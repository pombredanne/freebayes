// 
// freebayes
//
// A bayesian genetic variant detector.
// 

// standard includes
//#include <cstdio>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <time.h>
#include <float.h>

// private libraries
#include "BamReader.h"
#include "Fasta.h"
#include "TryCatch.h"
#include "Parameters.h"
#include "Allele.h"
#include "Sample.h"
#include "AlleleParser.h"
#include "Utility.h"

#include "multichoose.h"
#include "multipermute.h"

#include "Genotype.h"
#include "GenotypePriors.h"
#include "DataLikelihood.h"
#include "Marginals.h"
#include "ResultData.h"


// local helper debugging macros to improve code readability
#define DEBUG(msg) \
    if (parameters.debug) { cerr << msg << endl; }

// lower-priority messages
#ifdef VERBOSE_DEBUG
#define DEBUG2(msg) \
    if (parameters.debug2) { cerr << msg << endl; }
#else
#define DEBUG2(msg)
#endif

// must-see error messages
#define ERROR(msg) \
    cerr << msg << endl;


using namespace std; 


// freebayes main
int main (int argc, char *argv[]) {

    AlleleParser* parser = new AlleleParser(argc, argv);
    Parameters& parameters = parser->parameters;
    list<Allele*> alleles;

    Samples samples;

    ostream& out = *(parser->output);

    // this can be uncommented to force operation on a specific set of genotypes
    vector<Allele> allGenotypeAlleles;
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "A", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "T", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "G", 1));
    allGenotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "C", 1));

    int allowedAlleleTypes = ALLELE_REFERENCE;
    if (parameters.allowSNPs) {
        allowedAlleleTypes |= ALLELE_SNP;
    }
    if (parameters.allowIndels) {
        allowedAlleleTypes |= ALLELE_INSERTION;
        allowedAlleleTypes |= ALLELE_DELETION;
    }
    if (parameters.allowMNPs) {
        allowedAlleleTypes |= ALLELE_MNP;
    }

    // output VCF header
    if (parameters.output == "vcf") {
        vcfHeader(out, parser->reference.filename, parser->sampleList, parameters);
    }

    unsigned long total_sites = 0;
    unsigned long processed_sites = 0;

    while (parser->getNextAlleles(samples, allowedAlleleTypes)) {

        ++total_sites;

        DEBUG2("at start of main loop");

        // don't process non-ATGC's
        string cb = parser->currentReferenceBaseString();
        if (cb != "A" && cb != "T" && cb != "C" && cb != "G") {
            DEBUG2("current reference base is N");
            continue;
        }

        //filterAlleles(alleles, allowedAlleles);
        //removeIndelMaskedAlleles(alleles, parser->currentPosition);
        
        if (parameters.trace) {
            for (Samples::iterator s = samples.begin(); s != samples.end(); ++s) {
                const string& name = s->first;
                for (Sample::iterator g = s->second.begin(); g != s->second.end(); ++g) {
                    vector<Allele*>& group = g->second;
                    for (vector<Allele*>::iterator a = group.begin(); a != group.end(); ++a) {
                        Allele& allele = **a;
                        parser->traceFile << parser->currentTarget->seq << "," << (long unsigned int) parser->currentPosition + 1  
                            << ",allele," << name << "," << allele.readID << "," << allele.base() << ","
                            << allele.currentQuality() << "," << allele.mapQuality << endl;
                    }
                }
            }
            DEBUG2("after trace generation");
        }

        if (!parser->inTarget()) {
            DEBUG("position: " << parser->currentTarget->seq << ":" << (long unsigned int) parser->currentPosition + 1
                    << " is not inside any targets, skipping");
            continue;
        }

        int coverage = countAlleles(samples);

        DEBUG("position: " << parser->currentTarget->seq << ":" << (long unsigned int) parser->currentPosition + 1 << " coverage: " << coverage);

        // skips 0-coverage regions
        if (coverage == 0) {
            //cerr << "no alleles found at " << parser->currentTarget->seq << ":" << parser->currentPosition << endl;
            DEBUG("no alleles left at this site after filtering");
            continue;
        }

        DEBUG2("coverage " << parser->currentTarget->seq << ":" << parser->currentPosition << " == " << coverage);

        // establish a set of possible alternate alleles to evaluate at this location
        // only evaluate alleles with at least one supporting read with mapping
        // quality (MQL1) and base quality (BQL1)

        if (!sufficientAlternateObservations(samples, parameters.minAltCount, parameters.minAltFraction)) {
            DEBUG("insufficient alternate observations");
            continue;
        }

        map<string, vector<Allele*> > alleleGroups;
        groupAlleles(samples, alleleGroups);
        DEBUG2("grouped alleles by equivalence");

        int containedAlleleTypes = 0;
        for (map<string, vector<Allele*> >::iterator group = alleleGroups.begin(); group != alleleGroups.end(); ++group) {
            containedAlleleTypes |= group->second.front()->type;
        }

        // to ensure proper ordering of output stream
        vector<string> sampleListPlusRef;

        for (vector<string>::iterator s = parser->sampleList.begin(); s != parser->sampleList.end(); ++s) {
            sampleListPlusRef.push_back(*s);
        }
        if (parameters.useRefAllele)
            sampleListPlusRef.push_back(parser->currentTarget->seq);

        vector<Allele> genotypeAlleles = parser->genotypeAlleles(alleleGroups, samples, allGenotypeAlleles);

        if (genotypeAlleles.size() <= 1) { // if we have only one viable alternate, we don't have evidence for variation at this site
            DEBUG2("no alternate genotype alleles passed filters at " << parser->currentTarget->seq << ":" << parser->currentPosition);
            continue;
        }
        DEBUG2("genotype alleles: " << genotypeAlleles);

        ++processed_sites;

        // for each possible ploidy in the dataset, generate all possible genotypes
        map<int, vector<Genotype> > genotypesByPloidy;

        for (Samples::iterator s = samples.begin(); s != samples.end(); ++s) {
            string const& name = s->first;
            int samplePloidy = parser->currentSamplePloidy(name);
            if (genotypesByPloidy.find(samplePloidy) == genotypesByPloidy.end()) {
                DEBUG2("generating all possible genotypes for " << samplePloidy);
                genotypesByPloidy[samplePloidy] = allPossibleGenotypes(samplePloidy, genotypeAlleles);
                DEBUG2("done");
            }
        }

        DEBUG2("generated all possible genotypes:");
        if (parameters.debug2) {
            for (map<int, vector<Genotype> >::iterator s = genotypesByPloidy.begin(); s != genotypesByPloidy.end(); ++s) {
                vector<Genotype>& genotypes = s->second;
                for (vector<Genotype>::iterator g = genotypes.begin(); g != genotypes.end(); ++g) {
                    DEBUG2(*g);
                }
            }
        }

        Results results;

        DEBUG2("calculating data likelihoods");
        // calculate data likelihoods
        for (Samples::iterator s = samples.begin(); s != samples.end(); ++s) {

            string sampleName = s->first;
            Sample& sample = s->second;
            vector<Genotype>& genotypes = genotypesByPloidy[parser->currentSamplePloidy(sampleName)];

            vector<pair<Genotype*, long double> > probs = probObservedAllelesGivenGenotypes(sample, genotypes, parameters.RDF);

            map<Genotype*, long double> marginals;
            map<Genotype*, vector<long double> > rawMarginals;

            if (parameters.trace) {
                for (vector<pair<Genotype*, long double> >::iterator p = probs.begin(); p != probs.end(); ++p) {
                    parser->traceFile << parser->currentTarget->seq << "," << (long unsigned int) parser->currentPosition + 1 << ","
                        << sampleName << ",likelihood," << *(p->first) << "," << p->second << endl;
                }
            }

            results.insert(make_pair(sampleName, ResultData(sampleName, probs, marginals, rawMarginals, &sample)));

        }
        
        DEBUG2("finished calculating data likelihoods");

        // this section is a hack to make output of trace identical to BamBayes trace
        // and also outputs the list of samples
        vector<bool> samplesWithData;
        if (parameters.trace) {
            parser->traceFile << parser->currentTarget->seq << "," << (long unsigned int) parser->currentPosition + 1 << ",samples,";
            for (vector<string>::iterator s = sampleListPlusRef.begin(); s != sampleListPlusRef.end(); ++s) {
                if (parameters.trace) parser->traceFile << *s << ":";
                Results::iterator r = results.find(*s);
                if (r != results.end()) {
                    samplesWithData.push_back(true);
                } else {
                    samplesWithData.push_back(false);
                }
            }
            parser->traceFile << endl;
        }


        // sort individual genotype data likelihoods
        
        vector<pair<string, vector<pair<Genotype*, long double> > > > sampleGenotypes;
        for (vector<string>::iterator s = sampleListPlusRef.begin(); s != sampleListPlusRef.end(); ++s) {
            Results::iterator r = results.find(*s);
            if (r != results.end()) {
                r->second.sortDataLikelihoods();
                sampleGenotypes.push_back(make_pair(r->first, r->second.dataLikelihoods));
            }
        }

        DEBUG2("finished sorting data likelihoods");

        // calculate genotype combo likelihoods, integral over nearby genotypes
        // calculate marginals
        // and determine best genotype combination

        //DEBUG2("generating banded genotype combinations from " << genotypes.size() << " genotypes and " << sampleGenotypes.size() << " sample genotypes");
        DEBUG2("generating banded genotype combinations");
        vector<GenotypeCombo> bandedCombos;
        bandedGenotypeCombinationsIncludingAllHomozygousCombos(
                bandedCombos,
                sampleGenotypes,
                genotypesByPloidy,
                genotypeAlleles,
                parameters.WB,
                parameters.TB,
                parameters.genotypeComboStepMax);

        vector<GenotypeComboResult> genotypeComboProbs;

        Allele refAllele = genotypeAllele(ALLELE_REFERENCE, string(1, parser->currentReferenceBase), 1);

        DEBUG2("calculating genotype combination likelihoods");

        genotypeCombinationsPriorProbability(genotypeComboProbs, bandedCombos, refAllele, parameters.TH, parameters.pooled, parameters.diffusionPriorScalar);

        // sort by the normalized datalikelihood + prior
        DEBUG2("sorting genotype combination likelihoods");
        GenotypeComboResultSorter gcrSorter;
        sort(genotypeComboProbs.begin(), genotypeComboProbs.end(), gcrSorter);

        // resize to include only K chains, keeping us in O(NK) space instead
        // of O(N^2) for the ensuing calculations
        if (parameters.posteriorIntegrationDepth > 0) {
            deque<GenotypeComboResult> homozygousCombos;
            while (genotypeComboProbs.size() + homozygousCombos.size() > parameters.posteriorIntegrationDepth) {
                if (genotypeComboProbs.back().combo->isHomozygous()) {
                    homozygousCombos.push_back(genotypeComboProbs.back());
                }
                genotypeComboProbs.erase(genotypeComboProbs.end() - 1);
            }
            while (homozygousCombos.size() > 0) {
                genotypeComboProbs.push_back(homozygousCombos.front());
                homozygousCombos.pop_front();
            }
            // sort the homozygous chains into the right place
            sort(genotypeComboProbs.begin(), genotypeComboProbs.end(), gcrSorter);
        }
        
        // get posterior normalizer
        vector<long double> comboProbs;
        //comboProbs.resize(genotypeComboProbs.size());
        for (vector<GenotypeComboResult>::iterator gc = genotypeComboProbs.begin(); gc != genotypeComboProbs.end(); ++gc) {
            comboProbs.push_back(gc->priorComboProb);
        }
        long double posteriorNormalizer = logsumexp_probs(comboProbs);

        DEBUG2("got posterior normalizer");
        if (parameters.trace) {
            parser->traceFile << parser->currentTarget->seq << "," 
                << (long unsigned int) parser->currentPosition + 1 << ",posterior_normalizer," << posteriorNormalizer << endl;
        }

        // normalize marginals
        // note that this operation is O(N^2) in the number of combinations which we still
        // have after trimming the number of combos to parameters.posteriorIntegrationDepth

        DEBUG2("calculating marginal likelihoods");

        marginalGenotypeLikelihoods(posteriorNormalizer, genotypeComboProbs, results);

        // we provide p(var|data), or the probability that the location has
        // variation between individuals relative to the probability that it
        // has no variation
        //
        // in other words:
        // p(var|d) = 1 - p(AA|d) - p(TT|d) - P(GG|d) - P(CC|d)
        //
        // the approach is go through all the homozygous combos
        // and then subtract this from 1... resolving p(var|d)

        long double pVar = 1.0;

        bool hasHetCombo = false;
        GenotypeCombo* bestCombo = NULL;
        long double bestComboProb;

        for (vector<GenotypeComboResult>::iterator gc = genotypeComboProbs.begin(); gc != genotypeComboProbs.end(); ++gc) {
            if (gc->combo->isHomozygous()) {
                pVar -= safe_exp(gc->priorComboProb - posteriorNormalizer);
            } else if (!hasHetCombo) {
                bestCombo = gc->combo;
                bestComboProb = genotypeComboProbs.front().priorComboProb;
                hasHetCombo = true;
            }
        }

        if (!hasHetCombo) {
            bestCombo = genotypeComboProbs.front().combo;
            bestComboProb = genotypeComboProbs.front().priorComboProb;
        }

        DEBUG2("calculated pVar");

        // get the best heteroz
        GenotypeCombo& bestGenotypeCombo = *bestCombo; //*besthc;
        long double bestGenotypeComboProb = bestComboProb;
        long double bestGenotypeComboAlleleSamplingProb = safe_exp(alleleFrequencyProbabilityln(bestGenotypeCombo.countFrequencies(), parameters.TH));

        if (parameters.trace) {
            for (vector<GenotypeComboResult>::iterator gc = genotypeComboProbs.begin(); gc != genotypeComboProbs.end(); ++gc) {
                vector<Genotype*> comboGenotypes;
                for (GenotypeCombo::iterator g = gc->combo->begin(); g != gc->combo->end(); ++g)
                    comboGenotypes.push_back(g->genotype);
                long double priorComboProb = gc->priorComboProb;
                long double dataLikelihoodln = gc->probObsGivenGenotypes;
                long double priorln = gc->priorProbGenotypeCombo;
                long double priorlnG_Af = gc->priorProbGenotypeComboG_Af;
                long double priorlnAf = gc->priorProbGenotypeComboAf;

                parser->traceFile << parser->currentTarget->seq << "," << (long unsigned int) parser->currentPosition + 1 << ",genotypecombo,";

                int j = 0;
                GenotypeCombo::iterator i = gc->combo->begin();
                for (vector<bool>::iterator d = samplesWithData.begin(); d != samplesWithData.end(); ++d) {
                    if (*d) {
                        parser->traceFile << IUPAC(*i->genotype);
                        ++i;
                    } else {
                        parser->traceFile << "?";
                    }
                }
                    //<< "," << gc->first
                parser->traceFile 
                    << "," << dataLikelihoodln
                    << "," << priorln
                    << "," << priorlnG_Af
                    << "," << priorlnAf
                    << "," << dataLikelihoodln + priorln
                    << "," << safe_exp(dataLikelihoodln + priorln - posteriorNormalizer)
                    << endl;
            }
        }

        DEBUG2("got bestAlleleSamplingProb");
        DEBUG2("pVar = " << pVar << " " << parameters.PVL);

        if (!parameters.suppressOutput) {

            if (parameters.output == "json") {
                out << "{ \"position\": " << parser->currentPosition + 1 // 1-based reporting, to match vcf
                    << ", \"sequence\": " << parser->currentTarget->seq
                    << ", \"best_genotype_combo\":" << bestGenotypeCombo
                    << ", \"best_genotype_combo_prob\":" << bestGenotypeComboProb 
                    << ", \"best_genotype_combo_ewens_sampling_probability\":" << bestGenotypeComboAlleleSamplingProb
                    << ", \"combos_tested\":" << bandedCombos.size()
                    << ", \"coverage\":" << coverage
                    << ", \"posterior_normalizer\":" << safe_exp(posteriorNormalizer)
                    << ", \"samples\":";
                json(out, results, parser);
                out << "}" << endl;
            }
            if (pVar >= parameters.PVL) {
                if (parameters.output == "vcf") {
                    string referenceBase(1, parser->currentReferenceBase);
                    // get the unique alternate alleles in this combo, sorted by frequency in the combo
                    vector<pair<Allele, int> > alternates = alternateAlleles(bestGenotypeCombo, referenceBase);
                    if (parameters.reportAllAlternates) {
                        for (vector<pair<Allele, int> >::iterator a = alternates.begin(); a != alternates.end(); ++a) {
                            Allele& alt = a->first;
                            out << vcf(pVar,
                                    samples,
                                    referenceBase,
                                    alt.base(),
                                    alt,
                                    parser->sampleList,
                                    coverage,
                                    bestGenotypeCombo,
                                    results,
                                    parser)
                                << endl;
                        }
                    } else {
                        Allele& bestAlt = alternates.front().first;
                        // TODO update the vcf output function to handle the reporting of multiple alternate alleles
                        out << vcf(pVar,
                                samples,
                                referenceBase,
                                bestAlt.base(),
                                bestAlt,
                                parser->sampleList,
                                coverage,
                                bestGenotypeCombo,
                                results,
                                parser)
                            << endl;
                    }
                }
            } else if (!parameters.failedFile.empty()) {
                // XXX don't repeat yourself
                // get the unique alternate alleles in this combo, sorted by frequency in the combo
                long unsigned int position = parser->currentPosition;
                for (vector<Allele>::iterator ga =  genotypeAlleles.begin(); ga != genotypeAlleles.end(); ++ga) {
                    if (ga->type == ALLELE_REFERENCE)
                        continue;
                    parser->failedFile
                        << parser->currentTarget->seq << "\t"
                        << position << "\t"
                        << position + ga->length << "\t"
                        << *ga << endl;
                }
                // BED format
            }
        }
        DEBUG2("finished position");

    }

    DEBUG("total sites: " << total_sites << endl
         << "processed sites: " << processed_sites << endl
         << "ratio: " << (float) processed_sites / (float) total_sites);

    delete parser;

    return 0;

}
