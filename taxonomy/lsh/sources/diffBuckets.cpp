/*
 * Copyright (c) 2004-2005 Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * MIT grants permission to use, copy, modify, and distribute this software and
 * its documentation for NON-COMMERCIAL purposes and without fee, provided that
 * this copyright notice appears in all copies.
 *
 * MIT provides this software "as is," without representations or warranties of
 * any kind, either expressed or implied, including but not limited to the
 * implied warranties of merchantability, fitness for a particular purpose, and
 * noninfringement.  MIT shall not be liable for any damages arising from any
 * use of this software.
 *
 * Author: Alexandr Andoni (andoni@mit.edu), Piotr Indyk (indyk@mit.edu)
 * Modified by: Stephane Glondu (stephane.glondu@dptinfo.ens-cachan.fr)
 */

/*
  The main entry file containing the main() function. The main()
  function parses the command line parameters and depending on them
  calls the corresponding functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/times.h>
#include <sys/types.h>
#include <ctype.h>
#include <regex.h>
#include<set>
#include "headers.h"

using namespace std;

// Tranforming <memRatiosForNNStructs> from
// <memRatiosForNNStructs[i]=ratio of mem/total mem> to
// <memRatiosForNNStructs[i]=ratio of mem/mem left for structs i,i+1,...>.
void transformMemRatios(){
  RealT sum = 0;
  for(IntT i = nRadii - 1; i >= 0; i--){
    sum += memRatiosForNNStructs[i];
    memRatiosForNNStructs[i] = memRatiosForNNStructs[i] / sum;
    //DPRINTF("%0.6lf\n", memRatiosForNNStructs[i]);
  }
  ASSERT(sum <= 1.000001);
}

#define ENUM_BUCKETS

inline bool pointIsNotFiltered(PPointT bucketEle,PPointT queryPoint,set<int> templates) {
    return (bucketEle->iprop[ENUM_PPROP_TID] != queryPoint->iprop[ENUM_PPROP_TID]) &&
    (templates.count(bucketEle->iprop[ENUM_PPROP_TID]) == 0);
}

void computeParametersAndPrepare(bool computeParameters, char* paramsFile, PPointT * dataSetPoints, PPointT* sampleQueries) {
  
    if(!computeParameters) {
        computeParameters = readParamsFile(paramsFile,dataSetPoints);
    } 
    if (computeParameters) {
    Int32T sampleQBoundaryIndeces[nSampleQueries];
    // Compute the array sampleQBoundaryIndeces that specifies how to
    // segregate the sample query points according to their distance
    // to NN.
    sortQueryPointsByRadii(pointsDimension,
                           nSampleQueries,
                           sampleQueries,
                           nPoints,
                           dataSetPoints,
                           nRadii,
                           listOfRadii,
                           sampleQBoundaryIndeces);

    // Compute the R-NN DS parameters
    // if a parameter file is given, output them to that file, and continue
    // otherwise, output them to stdout, and exit

    FILE *fd;
    if (paramsFile == NULL) {
        fd = stdout;
    } else {
        fd = fopen(paramsFile, "wt");
        if (fd == NULL) {
            fprintf(stderr, "Unable to write to parameter file %s\n", paramsFile);
            exit(1);
        }
    }

    fprintf(fd, "%d\n", nRadii);
    transformMemRatios();
    
    for(IntT i = 0; i < nRadii; i++) {
      // which sample queries to use
        Int32T segregatedQStart = (i == 0) ? 0 : sampleQBoundaryIndeces[i - 1];
        Int32T segregatedQNumber = nSampleQueries - segregatedQStart;
        if (segregatedQNumber == 0) {
            // XXX: not the right answer
            segregatedQNumber = nSampleQueries;
            segregatedQStart = 0;
        }
        ASSERT(segregatedQStart < nSampleQueries);
        ASSERT(segregatedQStart >= 0);
        ASSERT(segregatedQStart + segregatedQNumber <= nSampleQueries);
        ASSERT(segregatedQNumber >= 0);
        RNNParametersT optParameters = computeOptimalParameters(listOfRadii[i],
                                                                successProbability,
                                                                nPoints,
                                                                pointsDimension,
                                                                dataSetPoints,
                                                                segregatedQNumber,
                                                                sampleQueries + segregatedQStart,
                                                                (Uns32T)((availableTotalMemory - totalAllocatedMemory) * memRatiosForNNStructs[i]));
        printRNNParameters(fd, optParameters);
    }
    if (fd == stdout) {
      exit(0);
    } else {
      fclose(fd);
      ASSERT(!readParamsFile(paramsFile,dataSetPoints));
    }

  }
  printf("========================= Structure built =========================\n");
  printf("nPoints = %d, Dimension = %d\n", nPoints, pointsDimension);
  printf("lowerBound = %d, upperBound = %d\n", lowerBound, upperBound);

}

void computeVectorClusters(PPointT * dataSetPoints) {
  // output vector clusters according to the filtering parameters.
  // FIXME: setting lower bound to 1 for now
    lowerBound = 1;
  PResultPointT *result = (PResultPointT*)MALLOC(nPoints * sizeof(PResultPointT));
  PPointT queryPoint;
  FAILIF(NULL == (queryPoint = (PPointT)MALLOC(sizeof(PointT))));
  FAILIF(NULL == (queryPoint->coordinates = (RealT*)MALLOC(pointsDimension * sizeof(RealT))));

  TimeVarT meanQueryTime = 0;
  int nQueries = 0;
  bool seen[nPoints];
  int nBuckets = 0, nBucketedPoints = 0;

  memset(seen, 0, nPoints * sizeof(bool));
  for(IntT i = 0; i < nPoints; nQueries++, i++) {
      // find the next unseen point
      while (i < nPoints && seen[i]) i++;
      if (i >= nPoints) break;
      queryPoint = dataSetPoints[i];
      // get the near neighbors.
      IntT nNNs = 0;
      for(IntT r = 0; r < nRadii; r++) { // nRadii is always 1 so far.
          nNNs = getRNearNeighbors(nnStructs[r], queryPoint, result, nPoints);
          meanQueryTime += timeRNNQuery;

          qsort(result, nNNs, sizeof(*result), comparePoints);
          set<int> templatesSeen;

          PResultPointT * cur = result, * end = result + nNNs;

          while (cur < end)  {
              ASSERT(cur != NULL);
                  
              // Look for the first un-filtered point for the next bucket.
              while ( cur < end ) {
                  if ( pointIsNotFiltered(cur->point,queryPoint,templatesSeen) ) {
                      templatesSeen.insert(cur->point->iprop[ENUM_PPROP_TID]);
                      break;
                  }
                  seen[cur->point->index] = true;
                  cur++;
              }
              if ( cur >= end )
                break;

              int sizeBucket = 1; // the first un-filtered point, which excludes the query point
              PResultPointT *begin = cur;
              seen[begin->point->index] = true;
              cur++;
              

              while (cur < end) {
                  if ( pointIsNotFiltered(cur->point,queryPoint,templatesSeen) ) {
                      templatesSeen.insert(cur->point->iprop[ENUM_PPROP_TID]);
                      sizeBucket++;
                  }
                  seen[cur->point->index] = true;
                  cur++;
              }

              templatesSeen.clear();
              // output the bucket if:
              //   - there are >= 2 different points
              //   - there are <= upperBound (default 0) && >= lowerBound (default 2) points
              //   - there are >= 2 different numbers of variables
              // and update nBuckets and nBucketedPoints consequently
              if (sizeBucket >= lowerBound && (upperBound < lowerBound || sizeBucket <= upperBound)) {
                  nBuckets++;
                  printf("\nQuery point %d: ",i);
                  printPoint(queryPoint);

                  printf("Bucket size %d, found %d NNs at distance %0.6lf (radius no. %d). NNs are:\n",
                         sizeBucket, nNNs, (double)(listOfRadii[r]), r);
                  for (PResultPointT *p = begin; p < cur; p++)  {
                      ASSERT(p != NULL);
                      nBucketedPoints++;
                      if(pointIsNotFiltered(p->point,queryPoint,templatesSeen)) {
                          templatesSeen.insert(p->point->iprop[ENUM_PPROP_TID]);
                      printf("%05d\tdist:%0.1lf \tTID:%d\tFILE %s\tREVNUM: %d\tMSG:%s\n", 
                             p->point->index, sqrt(p->distance),
                             p->point->iprop[ENUM_PPROP_TID],
                             p->point->cprop[ENUM_CPROP_FILE],
                             p->point->iprop[ENUM_PPROP_REVNUM],
                             p->point->cprop[ENUM_CPROP_MSG]);
                      }
                  }
              } // end of enumeration of a bucket
          } // for (...nRadii...)
      }
  }
  // Simple statistics and finish
  if (nQueries > 0) {
      meanQueryTime = meanQueryTime / nQueries;
      printf("\n%d queries, Mean query time: %0.6lf\n", nQueries, (double)meanQueryTime);
      printf("%d buckets, %d points (out of %d, %.2f %%) in them\n",
             nBuckets, nBucketedPoints, nPoints, 100*(float)nBucketedPoints/(float)nPoints);
  } 
}

class PointComp {
public:
    bool operator () (PointT lhs, PointT rhs) 
    { return lhs.index < rhs.index; }
};

class PResultPointComp {
public: 
    bool operator () (PResultPointT lhs, PResultPointT rhs) 
    { return lhs.point->iprop[ENUM_PPROP_TID] < rhs.point->iprop[ENUM_PPROP_TID]; }
};

typedef struct TResultEle_s {
    int templateID;
    set<PointT,PointComp> * queryPoints;
    set<PResultPointT,PResultPointComp> * neighbors;
    TResultEle_s * next;
    TResultEle_s * prev;
} TResultEle;

void computeClustersAndGroup(PPointT * dataSetPoints) {
  // output vector clusters according to the filtering parameters.
  // FIXME: setting lower bound to 1 for now
    lowerBound = 1;
  PResultPointT *result = (PResultPointT*)MALLOC(nPoints * sizeof(PResultPointT));
  PPointT queryPoint;
  FAILIF(NULL == (queryPoint = (PPointT)MALLOC(sizeof(PointT))));
  FAILIF(NULL == (queryPoint->coordinates = (RealT*)MALLOC(pointsDimension * sizeof(RealT))));

  TimeVarT meanQueryTime = 0;
  int nQueries = 0;
  bool seen[nPoints];
  int nBuckets = 0, nBucketedPoints = 0;
// for each template ID, find all neighbors in a bucket, put in its own bucket.  Output all at the end.

  TResultEle * buckets = NULL, * currentResult = NULL, *walker = NULL;

  memset(seen, 0, nPoints * sizeof(bool));
  for(IntT i = 0; i < nPoints; nQueries++, i++) {
      // find the next unseen point
      while (i < nPoints && seen[i]) i++;
      if (i >= nPoints) break;
      queryPoint = dataSetPoints[i];

      walker = buckets;

      while(walker != NULL && walker->templateID < queryPoint->iprop[ENUM_PPROP_TID]) {
          walker = walker->next;
      } 
      if(walker == NULL || walker->templateID != queryPoint->iprop[ENUM_PPROP_TID]) {
          currentResult = (TResultEle *) MALLOC(sizeof(TResultEle));
          currentResult->templateID = queryPoint->iprop[ENUM_PPROP_TID];
          currentResult->queryPoints = new set<PointT,PointComp>();
          currentResult->neighbors = new set<PResultPointT,PResultPointComp>();
          if(buckets == NULL) {
              buckets = currentResult;
              currentResult->next = NULL;
          } else {
              if(walker == NULL) { // insert at the end.  Annoying.
                  walker=buckets;
                  while(walker->next != NULL) walker=walker->next;
              walker->next = currentResult;
              currentResult->prev = walker;
              currentResult->next = NULL;

              } else {
              // walker points to the thing we should insert before
              ASSERT(walker != NULL);
              if(walker->prev == NULL) {
                  buckets = currentResult;
              }
              currentResult->next = walker;
              currentResult->prev = walker->prev;
              walker->prev = currentResult;
              
              }
          }
      } else {
          ASSERT(walker->templateID == queryPoint->iprop[ENUM_PPROP_TID]);
          printf("walker result: %d\n", walker->templateID);
          currentResult = walker;
      }
      currentResult->queryPoints->insert(*queryPoint);
      // get the near neighbors.

      IntT nNNs = 0;
      for(IntT r = 0; r < nRadii; r++) { // nRadii is always 1 so far.
          nNNs = getRNearNeighbors(nnStructs[r], queryPoint, result, nPoints);
          meanQueryTime += timeRNNQuery;

          qsort(result, nNNs, sizeof(*result), comparePoints);

          PResultPointT * cur = result, * end = result + nNNs;

          while (cur < end)  {
              set<int> templatesSeen;
              ASSERT(cur != NULL);
                  
              if ( pointIsNotFiltered(cur->point,queryPoint,templatesSeen) ) {
                  templatesSeen.insert(cur->point->iprop[ENUM_PPROP_TID]);
                  currentResult->neighbors->insert(*cur);
              }
              seen[cur->point->index] = true;
              cur++;
          } // end of enumeration of a bucket
      } // for (...nRadii...)
  }
  // print groups now
  walker = buckets;
  while(walker != NULL) {
      PResultPointT blah;
      PointT indicativePoint;
      printf("Template %d: ", walker->templateID);
      printf("Indicative Query Point: ");
      indicativePoint = *(walker->queryPoints->begin());
      printPoint(&indicativePoint);
      printf("Neighbors: ");
      set<PResultPointT, PResultPointComp>::iterator it = walker->neighbors->begin();
      for(; it != walker->neighbors->end(); it++) {
          blah = *it; // C++ is the dumbest thing ever. 
          printPoint(blah.point);
      }
      walker = walker->next;
  }
// Simple statistics and finish

  if (nQueries > 0) {
      meanQueryTime = meanQueryTime / nQueries;
      printf("\n%d queries, Mean query time: %0.6lf\n", nQueries, (double)meanQueryTime);
      printf("%d buckets, %d points (out of %d, %.2f %%) in them\n",
             nBuckets, nBucketedPoints, nPoints, 100*(float)nBucketedPoints/(float)nPoints);
  } 
}

PPointT * generateSampleQueries(PPointT * dataSetPoints, char * queryFname) {
    PPointT * sampleQueries;
    FAILIF(NULL == (sampleQueries = (PPointT*)MALLOC(nSampleQueries * sizeof(PPointT))));

    if (queryFname == NULL){
        // Choose several data set points for the sample query points.
        for(IntT i = 0; i < nSampleQueries; i++){
            sampleQueries[i] = dataSetPoints[genRandomInt(0, nPoints - 1)];
        }
    } else {
        FILE *queryFile = fopen(queryFname, "rt");
        sampleQueries = readDataSetFromFile(queryFname,NULL,false);
    }
    return sampleQueries;

}
/*
  The main entry to LSH package. Depending on the command line
  parameters, the function computes the R-NN data structure optimal
  parameters and/or construct the R-NN data structure and runs the
  queries on the data structure.
 */
int main(int argc, char *argv[]){

  FAILIF(0 != regcomp(&preg[ENUM_CPROP_FILE], "FILE:([^,]+)", REG_EXTENDED));
  FAILIF(0 != regcomp(&preg[ENUM_CPROP_MSG], "MSG:(\\{[^}]+\\})", REG_EXTENDED));
  FAILIF(0 != regcomp(&preg[ENUM_CPROP_BENCH], "BENCH:([^,]+)", REG_EXTENDED));
  FAILIF(0 != regcomp(&preg[ENUM_IPROP_TID], "TEMPLATEID:([^,]+)", REG_EXTENDED));
  FAILIF(0 != regcomp(&preg[ENUM_IPROP_REVNUM], "REVNUM:([^,]+)", REG_EXTENDED));
  FAILIF(0 != regcomp(&preg[ENUM_IPROP_LINESTART], "LINESTART:([^,]+)", REG_EXTENDED));
  FAILIF(0 != regcomp(&preg[ENUM_IPROP_LINEEND], "LINEEND:([^,]+)", REG_EXTENDED));

  //initializeLSHGlobal();
  availableTotalMemory = (unsigned int)8e8;  // 800MB by default

  // Parse part of the command-line parameters.
  bool computeParameters = false, group = false;
  char *paramsFile=NULL, *dataFile= NULL, *queryFile = NULL, *vec_files = NULL;
  // Parameters for filtering:

  for (int opt; (opt = getopt(argc, argv, "l:gs:q:p:P:R:cf:")) != -1; ) {
    // Needed: -p -f -R
    switch (opt) {
      case 'l': vec_files = optarg;
      case 's': nSampleQueries = atoi(optarg); 
      case 'q': queryFile = optarg; break;
      case 'p': paramsFile = optarg; break;
      case 'P': successProbability = atof(optarg); break;
      case 'c':
        fprintf(stderr, "Warning: will compute parameters\n");
        computeParameters = true;
        break;
      case 'R':
        nRadii = 1;
        FAILIF(NULL == (listOfRadii = (RealT*)MALLOC(nRadii * sizeof(RealT))));
        FAILIF(NULL == (memRatiosForNNStructs = (RealT*)MALLOC(nRadii * sizeof(RealT))));
        listOfRadii[0] = strtod(optarg, NULL);
        memRatiosForNNStructs[0] = 1;
        break;
      case 'f':
        printf("reading from file: %s\n", optarg);
        dataFile = optarg;
        break;
      case 'g': // group output by template
        group = true;
        break;
      default:
        fprintf(stderr, "Unknown option: -%c\n", opt);
        usage(1, argv[0]);
    }
  }

  if (optind < argc) {
    fprintf(stderr, "There are unprocessed parameters left\n");
    usage(1, argv[0]);
  }

  PPointT * dataSet = readDataSetFromFile(dataFile,vec_files,true);
  PPointT * sampleQueries = generateSampleQueries(dataSet, queryFile); 

  DPRINTF("Allocated memory (after reading data set): %d\n", totalAllocatedMemory);
  CHECK_INT(availableTotalMemory);
  CHECK_INT(nPoints);
  CHECK_INT(pointsDimension);
  CHECK_INT(nRadii);

  if (nPoints > MAX_N_POINTS) {
    printf("Error: the structure supports at most %d points (%d were specified).\n", MAX_N_POINTS, nPoints);
    fprintf(ERROR_OUTPUT, "Error: the structure supports at most %d points (%d were specified).\n", MAX_N_POINTS, nPoints);
    return 1;
  }

  computeParametersAndPrepare(computeParameters,paramsFile,dataSet,sampleQueries);
  printf("after compute parameters and prepare\n");
  if(!group)
    computeVectorClusters(dataSet);
  else
    computeClustersAndGroup(dataSet);
  return 0;
}
