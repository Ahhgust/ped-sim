// ped-sim: pedigree simulation tool
//
// This program is distributed under the terms of the GNU General Public License

#include <cstdio>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <random>
#include <sys/time.h>
#include "cmdlineopts.h"
#include "readdef.h"
#include "geneticmap.h"
#include "cointerfere.h"
#include "simulate.h"
#include "bpvcffam.h"
#include "ibdseg.h"
#include "fixedcos.h"

using namespace std;

int main(int argc, char **argv) {
  bool success = CmdLineOpts::parseCmdLineOptions(argc, argv);
  if (!success)
    return -1;

  // +13 for -everyone.fam, + 1 for \0 // +5 because we want to make - into stdout
  int outFileLen = strlen(CmdLineOpts::outPrefix) + 5 + 13 + 1;
  char *outFile = new char[outFileLen];
  if (outFile == NULL) {
    fprintf(stderr, "ERROR: out of memory");
    exit(5);
  }

  // open the log file
  sprintf(outFile, "%s.log", CmdLineOpts::outPrefix);
  if (CmdLineOpts::outPrefix[0] == '-' && CmdLineOpts::outPrefix[1] == 0)
    sprintf(outFile, "stdout.log");
  
  FILE *log = fopen(outFile, "w");
  if (!log) {
    fprintf(stderr, "ERROR: could not open log file %s!\n", outFile);
    perror("open");
    exit(1);
  }

  // seed random number generator if needed
  if (CmdLineOpts::autoSeed) {
    CmdLineOpts::randSeed = random_device().entropy();
    if (CmdLineOpts::randSeed == random_device().entropy()) {
      // random_device is not a real random number generator: fall back on
      // using time to generate a seed:
      timeval tv;
      gettimeofday(&tv, NULL);
      CmdLineOpts::randSeed = tv.tv_sec * tv.tv_usec;
    }
  }
  randomGen.seed(CmdLineOpts::randSeed);
  // changed from stdout (sorry, but this makes streaming the VCF to stdout feasible)
  FILE *outs[2] = { stderr, log };

  for(int o = 0; o < 2; o++) {
    fprintf(outs[o], "Pedigree simulator!  v%s    (Released %s)\n\n",
	    VERSION_NUMBER, RELEASE_DATE);

    fprintf(outs[o], "  Def file:\t\t%s\n", CmdLineOpts::defFile);
    fprintf(outs[o], "  Map file:\t\t%s\n", CmdLineOpts::mapFile);
    fprintf(outs[o], "  Input VCF:\t\t%s\n",
	    CmdLineOpts::inVCFfile == NULL ? "[none: no genetic data]" :
					     CmdLineOpts::inVCFfile);
    if (strcmp(CmdLineOpts::chrX, "X") != 0)
      fprintf(outs[o], "  X chromosome:\t\t%s\n\n", CmdLineOpts::chrX);
    fprintf(outs[o], "  Output prefix:\t%s\n\n", CmdLineOpts::outPrefix);

    fprintf(outs[o], "  Random seed:\t\t%u\n\n", CmdLineOpts::randSeed);

    if (CmdLineOpts::fixedCOfile)
      fprintf(outs[o], "  Fixed CO file:\t%s\n\n",
	      CmdLineOpts::fixedCOfile);
    else
      fprintf(outs[o], "  Interference file:\t%s\n\n",
	      CmdLineOpts::interfereFile == NULL ? "[none: Poisson model]" :
						    CmdLineOpts::interfereFile);

    if (CmdLineOpts::inVCFfile) {
      // options only relevant when generating data (so when we have input data)
      fprintf(outs[o], "  Genotype error rate:\t%.1le\n",
	      CmdLineOpts::genoErrRate);
      fprintf(outs[o], "  Opposite homozygous error rate:\t%.2lf\n",
	      CmdLineOpts::homErrRate);
      fprintf(outs[o], "  Missingness rate:\t%.1le\n",
	      CmdLineOpts::missRate);
      fprintf(outs[o], "  Pseudo-haploid rate:\t%.1lg\n",
	      CmdLineOpts::pseudoHapRate);
      fprintf(outs[o], "  Simulated coverage:\t%.1f\n",
              CmdLineOpts::coverage);
      fprintf(outs[o], "  Simulated quality:\t%.1f\n\n",
              CmdLineOpts::quality);

      if (CmdLineOpts::retainExtra < 0) {
	fprintf(outs[o], "  Retaining all unused samples in output VCF\n");
      }
      else if (CmdLineOpts::retainExtra == 0) {
	fprintf(outs[o], "  Not retaining extra samples in output VCF (printing only simulated samples)\n");
      }
      else {
	fprintf(outs[o], "  Retaining %d unused samples in output VCF\n",
		CmdLineOpts::retainExtra);
      }
      if (CmdLineOpts::keepPhase) {
	fprintf(outs[o], "  Output VCF will contain phased data\n\n");
      }
      else {
	fprintf(outs[o], "  Output VCF will contain unphased data\n\n");
      }
    }
  }

  vector<SimDetails> simDetails;
  readDef(simDetails, CmdLineOpts::defFile);

  bool sexSpecificMaps;
  GeneticMap map(CmdLineOpts::mapFile, sexSpecificMaps); // read the genetic map

  vector<COInterfere> coIntf;
  if (CmdLineOpts::interfereFile) {
    COInterfere::read(coIntf, CmdLineOpts::interfereFile, map, sexSpecificMaps);
  }

#ifndef NOFIXEDCO
  if (CmdLineOpts::fixedCOfile) {
    FixedCOs::read(CmdLineOpts::fixedCOfile, map);
  }
#endif // NOFIXEDCO

  unordered_map<const char*,uint8_t,HashString,EqString> sexes;
  uint32_t sexesCountData[2] = { 0, 0 };
  bool haveXmap = map.haveXmap();
  if (CmdLineOpts::inVCFfile) {
    if (!CmdLineOpts::vcfSexesFile && haveXmap) {
      for(int o = 0; o < 2; o++) {
	fprintf(outs[o], "WARNING: input VCF supplied and an X chromosome genetic map, but no --sexes file\n");
	fprintf(outs[o], "         output VCF will *not* include X chromosome data\n\n");
      }
    }
    else if (CmdLineOpts::vcfSexesFile)
      readSexes(sexes, sexesCountData, CmdLineOpts::vcfSexesFile);
  }

  // The first index is the pedigree number corresponding to the description of
  // the pedigree to be simulated in the def file
  // The second index is the family: we replicate the same pedigree structure
  // some number of times as specified in the def file
  // The third index is the generation number (0-based)
  // The fourth index is the branch of the pedigree
  // The fifth index is the individual number
  Person *****theSamples;

  // Record of which founders and descendants inherited a given haplotype along
  // with the start and end positions.
  // The first index is the haplotype number
  // The second index is the chromosome
  // The third index is the record index
  vector< vector< vector<InheritRecord> > > hapCarriers;

  for(int o = 0; o < 2; o++) {
    fprintf(outs[o], "Simulating haplotype transmissions... ");
    fflush(outs[o]);
  }
  vector<int> hapNumsBySex[2];
  int totalFounderHaps = simulate(simDetails, theSamples, map, sexSpecificMaps,
				  coIntf, hapCarriers, hapNumsBySex);
  for(int o = 0; o < 2; o++)
    fprintf(outs[o], "done.\n");

  if (sexesCountData[0] > 0 || sexesCountData[1] > 0) {
    if (sexesCountData[0] < hapNumsBySex[0].size() ||
				  sexesCountData[1] < hapNumsBySex[1].size()) {
      for(int o = 0; o < 2; o++) {
	fprintf(outs[o], "\n");
	fprintf(outs[o], "ERROR: need the input VCF to contain at least %lu females and %lu males, but\n",
		hapNumsBySex[1].size(), hapNumsBySex[0].size());
	fprintf(outs[o], "       the sexes file indicates there are %u females and %u males in the VCF\n",
		sexesCountData[1], sexesCountData[0]);
	fprintf(outs[o], "       Note: it is always possible to run without an input VCF or to get\n");
	fprintf(outs[o], "       autosomal genotypes by running without the --sexes option\n");
	exit(8);
      }
    }
  }

  if (CmdLineOpts::printBP) {
    for(int o = 0; o < 2; o++) {
      fprintf(outs[o], "Printing break points... ");
      fflush(outs[o]);
    }
    sprintf(outFile, "%s.bp", CmdLineOpts::outPrefix);
    if (CmdLineOpts::outPrefix[0] == '-' && CmdLineOpts::outPrefix[1] == 0)
      sprintf(outFile, "stdout.bp");

    
    printBPs(simDetails, theSamples, map, /*bpFile=*/ outFile);
    for(int o = 0; o < 2; o++) {
      fprintf(outs[o], "done.\n");
    }
  }

  for(int o = 0; o < 2; o++) {
    fprintf(outs[o], "Printing IBD segments");
    if (CmdLineOpts::printMRCA)
      fprintf(outs[o], " and MRCAs... ");
    else
      fprintf(outs[o], "... ");
    fflush(outs[o]);
  }
  sprintf(outFile, "%s.seg", CmdLineOpts::outPrefix);
  if (CmdLineOpts::outPrefix[0] == '-' && CmdLineOpts::outPrefix[1] == 0)
    sprintf(outFile, "stdout.seg");
  
  char *mrcaFile = NULL;
  if (CmdLineOpts::printMRCA) {
    mrcaFile = new char[outFileLen];
    if (mrcaFile == NULL) {
      printf("ERROR: out of memory");
      exit(5);
    }
    sprintf(mrcaFile, "%s.mrca", CmdLineOpts::outPrefix);
    if (CmdLineOpts::outPrefix[0] == '-' && CmdLineOpts::outPrefix[1] == 0)
      sprintf(mrcaFile, "stdout.mrca");
  }
  locatePrintIBD(simDetails, hapCarriers, map, sexSpecificMaps,
		 /*ibdFile=*/ outFile, /*ibdSegs=print them only=*/ NULL,
		 mrcaFile);
  for(int o = 0; o < 2; o++) {
    fprintf(outs[o], "done.\n");
  }

  if (CmdLineOpts::printFam) {
    for(int o = 0; o < 2; o++)
      fprintf(outs[o], "Printing fam file... ");
    fflush(stdout);
    sprintf(outFile, "%s-everyone.fam", CmdLineOpts::outPrefix);
    if (CmdLineOpts::outPrefix[0] == '-' && CmdLineOpts::outPrefix[1] == 0)
      sprintf(outFile, "stdout-everyone.fam");
    
    printFam(simDetails, theSamples, /*famFile=*/ outFile);
    for(int o = 0; o < 2; o++)
      fprintf(outs[o], "done.  (Do not use with PLINK data: see README.md)\n");
  }

  if (CmdLineOpts::inVCFfile) {
    for(int o = 0; o < 2; o++) {
      fprintf(outs[o], "Reading input VCF meta data... ");
      fflush(outs[o]);
    }
    // note: printVCF()'s callee makeVCF() print the status for generating the
    // VCF file

    int ret = printVCF(simDetails, theSamples, totalFounderHaps,
		       CmdLineOpts::inVCFfile, outFile, map, outs,
		       hapNumsBySex, sexes);

    if (ret == 0)
      for(int o = 0; o < 2; o++)
	fprintf(outs[o], "done.\n");
  }
  else {
    for(int o = 0; o < 2; o++)
      fprintf(outs[o], "\nTo simulate genetic data, must use an input VCF with %d founders.\n",
	      totalFounderHaps / 2);
  }

  fclose(log);

  // NOTE: the memory isn't freed because the OS reclaims it when Ped-sim
  // finishes

  return 0;
}
