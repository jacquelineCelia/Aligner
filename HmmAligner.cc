#include "HmmAligner.h"

using namespace std;
using namespace fst;
using namespace MultinomialParams;
// using namespace boost;

// initialize model 1 scores
HmmAligner::HmmAligner(const string& bitextFilename, 
		       const string& outputFilenamePrefix, 
		       const LearningInfo& learningInfo) {

  // Note: seed with time(0) if you don't care about reproducbility
  srand(425);

  // set member variables
  this->outputPrefix = outputFilenamePrefix;
  this->learningInfo = learningInfo;

  // encode training data
  vocabEncoder.useUnk = false;
  vocabEncoder.ReadParallelCorpus(bitextFilename, srcSents, tgtSents, NULL_SRC_TOKEN_STRING);
  assert(srcSents.size() > 0 && srcSents.size() == tgtSents.size());

  // initialize the model parameters
  // cerr << "rank #" << learningInfo.mpiWorld->rank() << ": init hmm params" << endl;
  cerr << "init hmm params" << endl;
  InitParams();
  /*
  if(learningInfo.mpiWorld->rank() == 0) {
    stringstream initialModelFilename;
    initialModelFilename << outputPrefix << ".param.init";
    PersistParams(initialModelFilename.str());
  }
  */
  stringstream initialModelFilename;
  initialModelFilename << outputPrefix << ".param.init";
  PersistParams(initialModelFilename.str());

  // create the initial grammar FST
  // cerr << "rank #" << learningInfo.mpiWorld->rank() << ": create grammar fst" << endl;
  //CreateGrammarFst();
  cerr << "create grammar fst" << endl;
  CreatePerSentGrammarFsts();
}

void HmmAligner::Train() {

  // create tgt fsts
  /*
  if(learningInfo.mpiWorld->rank() == 0) {
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": create tgt fsts" << endl;
  }
  */
  cerr << "create tgt fsts" << endl;
  vector< VectorFst <FstUtils::LogQuadArc> > tgtFsts;
  CreateTgtFsts(tgtFsts);

  // training iterations
  /*
  if(learningInfo.mpiWorld->rank() == 0) {
    cerr << "rank #" << learningInfo.mpiWorld->rank() << ": train!" << endl;
  }
  */
  cerr << "train!" << endl;
  LearnParameters(tgtFsts);
}

// src fsts are 1st order markov models
void HmmAligner::CreateSrcFsts(vector< VectorFst< FstUtils::LogQuadArc > >& srcFsts) {
  for(unsigned sentId = 0; sentId < srcSents.size(); sentId++) {
    vector< int > &intTokens = srcSents[sentId];
    if(intTokens[0] != NULL_SRC_TOKEN_ID) {
      intTokens.insert(intTokens.begin(), NULL_SRC_TOKEN_ID);
    }
    if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
      // cerr << "rank #" << learningInfo.mpiWorld->rank() << ": now creating src fst for sentence: ";
      cerr << "now creating src fst for sentence: ";
      for(unsigned i = 0; i < intTokens.size(); i++) {
	cerr << intTokens[i] << " ";
      }
      cerr << endl;
    }

    // create the fst
    VectorFst< FstUtils::LogQuadArc > srcFst;
    Create1stOrderSrcFst(intTokens, srcFst);
    srcFsts.push_back(srcFst);
  }
}

void HmmAligner::CreatePerSentGrammarFsts() {
  perSentGrammarFsts.clear();
  for(unsigned sentId = 0; sentId < srcSents.size(); sentId++) {
      vector<int> &srcTokens = srcSents[sentId];
      vector<int> &tgtTokens = tgtSents[sentId];
      assert(srcTokens[0] == NULL_SRC_TOKEN_ID);
      VectorFst< FstUtils::LogQuadArc > perSentGrammarFst;
      CreatePerSentGrammarFst(srcTokens, tgtTokens, perSentGrammarFst);
      perSentGrammarFsts.push_back(perSentGrammarFst);
  }
}

void HmmAligner::CreatePerSentGrammarFst(vector<int> &srcTokens, vector<int> &tgtTokensVector, VectorFst< FstUtils::LogQuadArc >& perSentGrammarFst) {
  
  set<int> tgtTokens(tgtTokensVector.begin(), tgtTokensVector.end());

  // allow null alignments
  assert(srcTokens[0] == NULL_SRC_TOKEN_ID);
    
  // create the fst
  int stateId = perSentGrammarFst.AddState();
  assert(stateId == 0);
  for(vector<int>::const_iterator srcTokenIter = srcTokens.begin(); srcTokenIter != srcTokens.end(); srcTokenIter++) {
    for(set<int>::const_iterator tgtTokenIter = tgtTokens.begin(); tgtTokenIter != tgtTokens.end(); tgtTokenIter++) {
      perSentGrammarFst.AddArc(stateId, FstUtils::LogQuadArc(*tgtTokenIter, *srcTokenIter, FstUtils::EncodeQuad(0, 0, 0, tFractionalCounts[*srcTokenIter][*tgtTokenIter]), stateId));	
    }
  }
  
  perSentGrammarFst.SetStart(stateId);
  perSentGrammarFst.SetFinal(stateId, FstUtils::LogQuadWeight::One());
  ArcSort(&perSentGrammarFst, ILabelCompare<FstUtils::LogQuadArc>());
}								       

// assumptions:
// - tgtFst is empty
void HmmAligner::CreateTgtFst(const vector<int> tgtTokens, VectorFst< FstUtils::LogQuadArc > &tgtFst) {
  assert(tgtFst.NumStates() == 0);
  int statesCount = tgtTokens.size() + 1;
  for(int stateId = 0; stateId < tgtTokens.size()+1; stateId++) {
    int temp = tgtFst.AddState();
    assert(temp == stateId);
    if(stateId == 0) continue;
    int tgtPos = stateId;
    tgtFst.AddArc(stateId-1, 
		  FstUtils::LogQuadArc(tgtTokens[stateId-1], 
			       tgtTokens[stateId-1], 
			       FstUtils::EncodeQuad(tgtPos, 0, 0, 0), 
			       stateId));
  }
  tgtFst.SetStart(0);
  tgtFst.SetFinal(tgtTokens.size(), FstUtils::LogQuadWeight::One());
  ArcSort(&tgtFst, ILabelCompare<FstUtils::LogQuadArc>());
}

void HmmAligner::CreateTgtFsts(vector< VectorFst< FstUtils::LogQuadArc > >& targetFsts) {
  // for each line
  for(unsigned sentId = 0; sentId < srcSents.size(); sentId++) {
    vector< int > &intTokens = tgtSents[sentId];
    
    // create the fst
    VectorFst< FstUtils::LogQuadArc > tgtFst;
    CreateTgtFst(intTokens, tgtFst);
    targetFsts.push_back(tgtFst);
  }
}

void HmmAligner::NormalizeFractionalCounts() {
  MultinomialParams::NormalizeParams(aFractionalCounts);
  MultinomialParams::NormalizeParams(tFractionalCounts);
}

void HmmAligner::PrintParams() {
  aParams.PrintParams();
  tFractionalCounts.PrintParams();
}

void HmmAligner::PersistParams(const string& outputFilename) {
  string translationFilename = outputFilename + string(".t");
  MultinomialParams::PersistParams(translationFilename, tFractionalCounts, vocabEncoder, true, true);
  string transitionFilename = outputFilename + string(".a");
  MultinomialParams::PersistParams(transitionFilename, aParams, vocabEncoder, false, false);
}

// finds out what are the parameters needed by reading hte corpus, and assigning initial weights based on the number of co-occurences
void HmmAligner::InitParams() {
  // for each parallel sentence
  for(int sentId = 0; sentId < srcSents.size(); sentId++) {

    // read the list of integers representing target tokens
    vector< int > &tgtTokens = tgtSents[sentId], &srcTokens = srcSents[sentId];
    
    // we want to allow target words to align to NULL (which has srcTokenId = 1).
    if(srcTokens[0] != NULL_SRC_TOKEN_ID) {
      srcTokens.insert(srcTokens.begin(), NULL_SRC_TOKEN_ID);
    }
    
    // for each srcToken
    for(int i=0; i<srcTokens.size(); i++) {

      // INITIALIZE TRANSLATION PARAMETERS
      int srcToken = srcTokens[i];
      // get the corresponding map of tgtTokens (and the corresponding probabilities)
      map<int, double> &tParamsGivenS_i = tFractionalCounts[srcToken];
      // for each tgtToken
      for (int j=0; j<tgtTokens.size(); j++) {
	int tgtToken = tgtTokens[j];
	// TODO: consider initializing these parameters with a uniform distribution instead of reflecting co-occurences. EM should figure it out on its own.
	// if this the first time the pair(tgtToken, srcToken) is experienced, give it a value of 1 (i.e. prob = exp(-1) ~= 1/3)
	if( tParamsGivenS_i.count(tgtToken) == 0) {
	  tParamsGivenS_i[tgtToken] = FstUtils::nLog(1/3.0);
	} else {
	  // otherwise, add nLog(1/3) to the original value, effectively counting the number of times 
	  // this srcToken-tgtToken pair appears in the corpus
	  tParamsGivenS_i[tgtToken] = Plus( FstUtils::LogWeight(tParamsGivenS_i[tgtToken]), FstUtils::LogWeight(FstUtils::nLog(1/3.0)) ).Value();
	}
	tParamsGivenS_i[tgtToken] = fabs(gaussianSampler.Draw());
      }

      // INITIALIZE ALIGNMENT PARAMETERS
      // TODO: It *might* be a good idea to initialize those parameters reflecting co-occurence statistics such that p(a=50|prev_a=30) < p(a=10|prev_a=30).
      //       EM should be able to figure it out on its own, though.
      for(int k=-1; k<srcTokens.size(); k++) {
      // assume that previous alignment = k, initialize p(i|k)
	aFractionalCounts[k][i] = FstUtils::nLog(1/3.0);
	aFractionalCounts[k][i] = gaussianSampler.Draw();
      }
      // also initialize aFractionalCounts[-1][i]
      aFractionalCounts[INITIAL_SRC_POS][i] = FstUtils::nLog(1/3.0);
      aFractionalCounts[INITIAL_SRC_POS][i] = gaussianSampler.Draw();
    }
  }
    
  NormalizeFractionalCounts();
  DeepCopy(aFractionalCounts, aParams);
}

// make a deep copy of parameters
void HmmAligner::DeepCopy(const ConditionalMultinomialParam<int>& original, 
			ConditionalMultinomialParam<int>& duplicate) {
  // zero duplicate
  MultinomialParams::ClearParams(duplicate);

  // copy original into duplicate
  for(map<int, MultinomialParams::MultinomialParam>::const_iterator contextIter = original.params.begin(); 
      contextIter != original.params.end();
      contextIter ++) {
    for(MultinomialParam::const_iterator multIter = contextIter->second.begin();
	multIter != contextIter->second.end();
	multIter ++) {
      duplicate[contextIter->first][multIter->first] = multIter->second;
    }
  }
}

/*
void HmmAligner::CreateGrammarFst() {
  // clear grammar
  if (grammarFst.NumStates() > 0) {
    grammarFst.DeleteArcs(grammarFst.Start());
    grammarFst.DeleteStates();
    assert(grammarFst.NumStates() == 0);
  }
  
  // create the only state in this fst, and make it initial and final
  FstUtils::LogQuadArc::StateId dummy = grammarFst.AddState();
  assert(dummy == 0);
  grammarFst.SetStart(0);
  grammarFst.SetFinal(0, FstUtils::LogQuadWeight::One());
  int fromState = 0, toState = 0;
  assert(tFractionalCounts.params.size() > 0);
  for(map<int, MultinomialParam>::const_iterator srcIter = tFractionalCounts.params.begin(); srcIter != tFractionalCounts.params.end(); srcIter++) {
    for(MultinomialParam::const_iterator tgtIter = (*srcIter).second.begin(); tgtIter != (*srcIter).second.end(); tgtIter++) {
      int tgtToken = (*tgtIter).first;
      int srcToken = (*srcIter).first;
      float paramValue = (*tgtIter).second;
      grammarFst.AddArc(fromState, 
			FstUtils::LogQuadArc(tgtToken, 
				     srcToken, 
				     FstUtils::EncodeQuad(0, 0, 0, paramValue), 
				     toState));
    }
  }
  ArcSort(&grammarFst, ILabelCompare<FstUtils::LogQuadArc>());
}
*/

// assumptions:
// - first token in srcTokens is the NULL token (to represent null-alignments)
// - srcFst is assumed to be empty
//
// notes:
// - the structure of this FST is laid out such that each state encodes the previous non-null 
//   src position. the initial state is unique: it represents both the starting state the state
//   where all previous alignments are null-alignments.
// - if a source type is repeated, it will have multiple states corresponding to the different positions
// - the "1stOrder" part of the function name indicates this FST represents a first order markov process
//   for alignment transitions.
//
void HmmAligner::Create1stOrderSrcFst(const vector<int>& srcTokens, VectorFst<FstUtils::LogQuadArc>& srcFst) {
  // enforce assumptions
  assert(srcTokens.size() > 0 && srcTokens[0] == NULL_SRC_TOKEN_ID);
  assert(srcFst.NumStates() == 0);

  // create one state per src position
  for(int i = 0; i < srcTokens.size(); i++) {
    int stateId = srcFst.AddState();
    // assumption that AddState() first returns a zero then increment ones
    assert(i == stateId);
  }

  // for each state
  for(int i = 0; i < srcTokens.size(); i++) {

    // for debugging only
    //    cerr << "srcTokens[" << i << "] = " << srcTokens[i] << endl;
    
    // set the initial/final states
    if(i == 0) {
      srcFst.SetStart(i);
    } else {
      srcFst.SetFinal(i, FstUtils::LogQuadWeight::One());
    }

    // we don't allow prevAlignment to be null alignment in our markov model. if a null alignment happens after alignment = 5, we use 5 as prevAlignment, not the null alignment. if null alignment happens before any non-null alignment, we use a special src position INITIAL_SRC_POS to indicate the prevAlignment
    int prevAlignment = i == 0? INITIAL_SRC_POS : i;

    // each state can go to itself with the null src token
    srcFst.AddArc(i, FstUtils::LogQuadArc(srcTokens[0], srcTokens[0], FstUtils::EncodeQuad(0, i, prevAlignment, aParams[prevAlignment][i]), i));

    // each state can go to states representing non-null alignments
    for(int j = 1; j < srcTokens.size(); j++) {
      srcFst.AddArc(i, FstUtils::LogQuadArc(srcTokens[j], srcTokens[j], FstUtils::EncodeQuad(0, j, prevAlignment, aParams[prevAlignment][j]), j));
    }
  }
 
  // arc sort to enable composition
  ArcSort(&srcFst, ILabelCompare<FstUtils::LogQuadArc>());

  // for debugging
  //  cerr << "=============SRC FST==========" << endl;
  //  cerr << FstUtils::PrintFstSummary(srcFst);
}

void HmmAligner::ClearFractionalCounts() {
  MultinomialParams::ClearParams(tFractionalCounts, learningInfo.smoothMultinomialParams);
  MultinomialParams::ClearParams(aFractionalCounts, learningInfo.smoothMultinomialParams);
}

void HmmAligner::BuildAlignmentFst(const VectorFst< FstUtils::LogQuadArc > &tgtFst,
				 const VectorFst< FstUtils::LogQuadArc > &perSentGrammarFst,
				 const VectorFst< FstUtils::LogQuadArc > &srcFst, 
				 VectorFst< FstUtils::LogQuadArc > &alignmentFst) {
  VectorFst< FstUtils::LogQuadArc > temp;
  Compose(tgtFst, perSentGrammarFst, &temp);
  Compose(temp, srcFst, &alignmentFst);  
}

void HmmAligner::LearnParameters(vector< VectorFst< FstUtils::LogQuadArc > >& tgtFsts) {
  clock_t compositionClocks = 0, forwardBackwardClocks = 0, updatingFractionalCountsClocks = 0, normalizationClocks = 0;
  clock_t t00 = clock();
  do {
    clock_t t05 = clock();

    // create src fsts (these encode the aParams as weights on their arcs)
    /*
    if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
      cerr << "rank #" << learningInfo.mpiWorld->rank() << ": create src fsts" << endl;
    }
    */
    cerr << "create src fsts" << endl;
    vector< VectorFst <FstUtils::LogQuadArc> > srcFsts;
    CreateSrcFsts(srcFsts);
    /*
    if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
      cerr << "rank #" << learningInfo.mpiWorld->rank() << ": created src fsts" << endl;
    }
    */
    cerr << "created src fsts" << endl;
    clock_t t10 = clock();
    float logLikelihood = 0, validationLogLikelihood = 0;
    
    // this vector will be used to accumulate fractional counts of parameter usages
    ClearFractionalCounts();
    /*
    if(learningInfo.debugLevel >= DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
      cerr << "rank #" << learningInfo.mpiWorld->rank() << ": cleared fractional counts vector" << endl;
    }
    */
    cerr << "cleared fractional counts vector" << endl;
    // iterate over sentences
    int sentsCounter = 0;
    for( vector< VectorFst< FstUtils::LogQuadArc > >::const_iterator tgtIter = tgtFsts.begin(), srcIter = srcFsts.begin(), perSentGrammarIter = perSentGrammarFsts.begin(); 
	 tgtIter != tgtFsts.end() && srcIter != srcFsts.end() && perSentGrammarIter != perSentGrammarFsts.end(); 
	 tgtIter++, srcIter++, perSentGrammarIter++, sentsCounter++) {

      // build the alignment fst
      clock_t t20 = clock();
      const VectorFst< FstUtils::LogQuadArc > &tgtFst = *tgtIter, &srcFst = *srcIter, &perSentGrammarFst = *perSentGrammarIter;
      VectorFst< FstUtils::LogQuadArc > alignmentFst;
      BuildAlignmentFst(tgtFst, perSentGrammarFst,  srcFst, alignmentFst);
      compositionClocks += clock() - t20;
      if(learningInfo.debugLevel >= DebugLevel::SENTENCE) {
	cerr << "built alignment fst. |tgtFst| = " << tgtFst.NumStates() << ", |srcFst| = " << srcFst.NumStates() << ", |alignmentFst| = " << alignmentFst.NumStates() << endl;
	cerr << "===SRC FST===" << endl << FstUtils::PrintFstSummary<FstUtils::LogQuadArc>(srcFst);
	cerr << "===TGT FST===" << endl << FstUtils::PrintFstSummary<FstUtils::LogQuadArc>(tgtFst);
	cerr << "===ALIGNMENT FST===" << endl << FstUtils::PrintFstSummary<FstUtils::LogQuadArc>(alignmentFst);
      }
      
      // run forward/backward for this sentence
      clock_t t30 = clock();
      vector<FstUtils::LogQuadWeight> alphas, betas;
      ShortestDistance(alignmentFst, &alphas, false);
      ShortestDistance(alignmentFst, &betas, true);
      if(learningInfo.debugLevel >= DebugLevel::SENTENCE) {
	cerr << "shortest distance (both directions) was computed" << endl;
	cerr << "alignmentFst.Start() = " << alignmentFst.Start() << endl;
	cerr << "betas[alignmentFst.Start()] = " << FstUtils::PrintWeight( betas[alignmentFst.Start()] ) << endl;
      }
      float fSentLogLikelihood, dummy;
      FstUtils::DecodeQuad(betas[alignmentFst.Start()], 
			     dummy, dummy, dummy, fSentLogLikelihood);
      if(std::isnan(fSentLogLikelihood) || std::isinf(fSentLogLikelihood)) {
	// cerr << "rank #" << learningInfo.mpiWorld->rank() << ": sent #" << sentsCounter << " give a sent loglikelihood of " << fSentLogLikelihood << endl;
	cerr << ": sent #" << sentsCounter << " give a sent loglikelihood of " << fSentLogLikelihood << endl;
	assert(false);
      }
      forwardBackwardClocks += clock() - t30;
      if(learningInfo.debugLevel >= DebugLevel::SENTENCE) {
	cerr << "fSentLogLikelihood = " << fSentLogLikelihood << endl;
      }
      
      // compute and accumulate fractional counts for model parameters
      clock_t t40 = clock();
      /*
      if(learningInfo.debugLevel >= DebugLevel::SENTENCE) {
	cerr << "rank #" << learningInfo.mpiWorld->rank() << ": sentsCounter = " << sentsCounter << endl;
      }
      */
      bool excludeFractionalCountsInThisSent = 
	learningInfo.useEarlyStopping && 
	sentsCounter % learningInfo.trainToDevDataSize == 0;
      for (int stateId = 0; !excludeFractionalCountsInThisSent && stateId < alignmentFst.NumStates() ;stateId++) {
	for (ArcIterator<VectorFst< FstUtils::LogQuadArc > > arcIter(alignmentFst, stateId);
	     !arcIter.Done();
	     arcIter.Next()) {

	  // decode arc information
	  int srcToken = arcIter.Value().olabel, tgtToken = arcIter.Value().ilabel;
	  int fromState = stateId, toState = arcIter.Value().nextstate;
	  float fCurrentTgtPos, fCurrentSrcPos, fPrevSrcPos, arcLogProb;
	  FstUtils::DecodeQuad(arcIter.Value().weight, fCurrentTgtPos, fCurrentSrcPos, fPrevSrcPos, arcLogProb);
	  int currentSrcPos = (int) fCurrentSrcPos, prevSrcPos = (int) fPrevSrcPos;

	  // probability of using this parameter given this sentence pair and the previous model
	  float alpha, beta, dummy;
	  FstUtils::DecodeQuad(alphas[fromState], dummy, dummy, dummy, alpha);
	  FstUtils::DecodeQuad(betas[toState], dummy, dummy, dummy, beta);
	  float fNormalizedPosteriorLogProb = (alpha + arcLogProb + beta) - fSentLogLikelihood;
	  if(std::isnan(fNormalizedPosteriorLogProb) || std::isinf(fNormalizedPosteriorLogProb)) {
	    // cerr << "rank #" << learningInfo.mpiWorld->rank() << ": sent #" << sentsCounter << " give a normalized posterior logprob of  " << fNormalizedPosteriorLogProb << endl;
	    cerr << ": sent #" << sentsCounter << " give a normalized posterior logprob of  " << fNormalizedPosteriorLogProb << endl;
	    assert(false);
	  }
	    
	  // update tFractionalCounts
	  tFractionalCounts[srcToken][tgtToken] = 
	    Plus(FstUtils::LogWeight(tFractionalCounts[srcToken][tgtToken]), 
		 FstUtils::LogWeight(fNormalizedPosteriorLogProb)).Value();
	  // update aFractionalCounts
	  aFractionalCounts[prevSrcPos][currentSrcPos] = 
	    Plus(FstUtils::LogWeight(aFractionalCounts[prevSrcPos][currentSrcPos]),
		 FstUtils::LogWeight(fNormalizedPosteriorLogProb)).Value();
	  
	}
      }
      updatingFractionalCountsClocks += clock() - t40;
      
      // update the iteration log likelihood with this sentence's likelihod
      if(excludeFractionalCountsInThisSent) {
	validationLogLikelihood += fSentLogLikelihood;
      } else {
	logLikelihood += fSentLogLikelihood;
	//cerr << "sent #" << sentsCounter << " likelihood = " << fSentLogLikelihood << " @ iteration " << learningInfo.iterationsCount << endl;
      }
      //	cout << "iteration's loglikelihood = " << logLikelihood << endl;
      
      // logging
      if (sentsCounter > 0 && sentsCounter % 1000 == 0 && learningInfo.debugLevel == DebugLevel::CORPUS) {
	cerr << ".";
      }
    }

    // if(learningInfo.debugLevel == DebugLevel::CORPUS && learningInfo.mpiWorld->rank() == 0) {
    if(learningInfo.debugLevel == DebugLevel::CORPUS) {
      cerr << "fractional counts collected from relevant sentences for this iteration." << endl;
    }
    
    // all processes send their fractional counts to the master and the master accumulates them
    /*
    boost::mpi::reduce<map<int, MultinomialParams::MultinomialParam> >(*learningInfo.mpiWorld, tFractionalCounts.params, tFractionalCounts.params, MultinomialParams::AccumulateConditionalMultinomialsLogSpace<int>, 0);
    boost::mpi::reduce<map<int, MultinomialParams::MultinomialParam> >(*learningInfo.mpiWorld, aFractionalCounts.params, aFractionalCounts.params, MultinomialParams::AccumulateConditionalMultinomialsLogSpace<int>, 0);
    boost::mpi::all_reduce<float>(*learningInfo.mpiWorld, logLikelihood, logLikelihood, std::plus<float>());
    */
    //cerr << "WHEN NP = " << learningInfo.mpiWorld->size() << ", total logLikelihood = " << logLikelihood << " at iteration #" << learningInfo.iterationsCount << endl; 

    // master only: normalize fractional counts such that \sum_t p(t|s) = 1 \forall s
    /*
    if(learningInfo.mpiWorld->rank() == 0) {
      clock_t t50 = clock();
      NormalizeFractionalCounts();
      DeepCopy(aFractionalCounts, aParams);
      normalizationClocks += clock() - t50;
    }
    */
    clock_t t50 = clock();
    NormalizeFractionalCounts();
    DeepCopy(aFractionalCounts, aParams);
    normalizationClocks += clock() - t50;

    // update a few things on slaves
    /*
    boost::mpi::broadcast< map<int, MultinomialParams::MultinomialParam > >(*learningInfo.mpiWorld, tFractionalCounts.params, 0);    
    boost::mpi::broadcast< map<int, MultinomialParams::MultinomialParam > >(*learningInfo.mpiWorld, aFractionalCounts.params, 0);    
    boost::mpi::broadcast< map<int, MultinomialParams::MultinomialParam > >(*learningInfo.mpiWorld, aParams.params, 0);    
    */
    // persist parameters, if need be
    if( learningInfo.iterationsCount % learningInfo.persistParamsAfterNIteration == 0 && 
	learningInfo.iterationsCount != 0) { 
	// && learningInfo.mpiWorld->rank() == 0) {
      cerr << "persisting params:" << endl;
      stringstream filename;
      filename << outputPrefix << ".param." << learningInfo.iterationsCount;
      PersistParams(filename.str());
    }
    
    // create the new grammar
    clock_t t60 = clock();
    CreatePerSentGrammarFsts();
    //CreateGrammarFst();

    // logging
    // cerr << "rank #" << learningInfo.mpiWorld->rank() << ": iterations # " << learningInfo.iterationsCount << " - total loglikelihood = " << logLikelihood << endl;
    cerr << "iterations # " << learningInfo.iterationsCount << " - total loglikelihood = " << logLikelihood << endl;
    
    // update learningInfo
    learningInfo.logLikelihood.push_back(logLikelihood);
    learningInfo.validationLogLikelihood.push_back(validationLogLikelihood);
    learningInfo.iterationsCount++;
    
    // check for convergence
  } while(!learningInfo.IsModelConverged());

  // logging
  if(learningInfo.debugLevel >= DebugLevel::REDICULOUS) {
    cerr << endl;
    cerr << "trainTime        = " << (float) (clock() - t00) / CLOCKS_PER_SEC << " sec." << endl;
    cerr << "compositionTime  = " << (float) compositionClocks / CLOCKS_PER_SEC << " sec." << endl;
    cerr << "forward/backward = " << (float) forwardBackwardClocks / CLOCKS_PER_SEC << " sec." << endl;
    cerr << "fractionalCounts = " << (float) updatingFractionalCountsClocks / CLOCKS_PER_SEC << " sec." << endl;
    cerr << "normalizeClocks  = " << (float) normalizationClocks / CLOCKS_PER_SEC << " sec." << endl;
    cerr << endl;
  }
}

// assumptions:
// - both aParams and tFractionalCounts are properly normalized logProbs
// sample both an alignment and a translation, given src sentence and tgt length
void HmmAligner::SampleATGivenS(const vector<int>& srcTokens, int tgtLength, vector<int>& tgtTokens, vector<int>& alignments, double& hmmLogProb) {

  // intialize
  int prevAlignment = INITIAL_SRC_POS;
  hmmLogProb = 0;

  // for each target position,
  for(int i = 0; i < tgtLength; i++) {
    // sample a src position (i.e. an alignment)
    int currentAlignment;
    do {
      currentAlignment = SampleFromMultinomial(aParams[prevAlignment]);
    } while(currentAlignment >= srcTokens.size());
    alignments.push_back(currentAlignment);
    // sample a translation
    int currentTranslation = SampleFromMultinomial(tFractionalCounts[srcTokens[currentAlignment]]);
    tgtTokens.push_back(currentTranslation);
    // for debugging only
    //cerr << "and sample translation=" << currentTranslation << endl;

    // update the sample probability according to the model
    hmmLogProb += aParams[prevAlignment][currentAlignment];
    hmmLogProb += tFractionalCounts[srcTokens[currentAlignment]][currentTranslation];

    // update prevAlignment
    if(currentAlignment != NULL_SRC_TOKEN_POS) {
      prevAlignment = currentAlignment;
    }
  }

	//cerr << "hmmLogProb=" << hmmLogProb << endl;
  assert(tgtTokens.size() == tgtLength && alignments.size() == tgtLength);
  assert(hmmLogProb >= 0);
}

// sample an alignment given a source sentence and a its translation.
void HmmAligner::SampleAGivenST(const std::vector<int> &srcTokens,
		    const std::vector<int> &tgtTokens,
		    std::vector<int> &alignments,
		    double &logProb) {
  cerr << "method not implemented" << endl;
  assert(false);
}

// given the current model, align a test sentence
// assumptions: 
// - the null token has *NOT* been inserted yet
string HmmAligner::AlignSent(vector<int> srcTokens, vector<int> tgtTokens) {
  
  static int sentCounter = 0;
  
  // make sure the source sentnece is sane
  assert(srcTokens.size() > 0 && srcTokens[0] == NULL_SRC_TOKEN_ID);
  
  // build aGivenTS
  VectorFst<FstUtils::LogQuadArc> tgtFst, srcFst, alignmentFst, perSentGrammarFst;
  CreateTgtFst(tgtTokens, tgtFst);  
  Create1stOrderSrcFst(srcTokens, srcFst);
  CreatePerSentGrammarFst(srcTokens, tgtTokens, perSentGrammarFst);
  BuildAlignmentFst(tgtFst, perSentGrammarFst, srcFst, alignmentFst);
  VectorFst< FstUtils::LogArc > alignmentFstProbs;
  ArcMap(alignmentFst, &alignmentFstProbs, FstUtils::LogQuadToLogPositionMapper());
  // tropical has the path property
  VectorFst< FstUtils::StdArc > alignmentFstProbsWithPathProperty, bestAlignment;
  ArcMap(alignmentFstProbs, &alignmentFstProbsWithPathProperty, FstUtils::LogToTropicalMapper());
  ShortestPath(alignmentFstProbsWithPathProperty, &bestAlignment);
  return FstUtils::PrintAlignment(bestAlignment);
}

void HmmAligner::AlignTestSet(const string &testBitextFilename, const string &outputAlignmentsFilename) {
  //const string &srcTestSetFilename, const string &tgtTestSetFilename, const string &outputAlignmentsFilename) {

  vector< vector<int> > srcTestSents, tgtTestSents;
  vocabEncoder.ReadParallelCorpus(testBitextFilename, srcTestSents, tgtTestSents, NULL_SRC_TOKEN_STRING);
  assert(srcTestSents.size() == tgtTestSents.size());
  
  ofstream outputAlignments(outputAlignmentsFilename.c_str(), ios::out);

  // for each parallel line
  for(unsigned sentId = 0; sentId < srcTestSents.size(); sentId++) {
    string alignmentsLine;
    vector< int > &srcTokens = srcTestSents[sentId], &tgtTokens = tgtTestSents[sentId];
    cout << "sent #" << sentId << " |srcTokens| = " << srcTokens.size() << endl;
    alignmentsLine = AlignSent(srcTokens, tgtTokens);
    outputAlignments << alignmentsLine;
  }
  outputAlignments.close();
}

void HmmAligner::Align() {
  Align(outputPrefix + ".train.align");
}

void HmmAligner::Align(const string &alignmentsFilename) {
  ofstream outputAlignments;
  /*
  if(learningInfo.mpiWorld->rank() == 0) {
    outputAlignments.open(alignmentsFilename.c_str(), ios::out);
  }
  */
  outputAlignments.open(alignmentsFilename.c_str(), ios::out);
  for(unsigned sentId = 0; sentId < srcSents.size(); sentId++) {
    string alignmentsLine;
    // if(sentId % learningInfo.mpiWorld->size() == learningInfo.mpiWorld->rank()) {
      vector<int> &srcSent = srcSents[sentId], &tgtSent = tgtSents[sentId];
      alignmentsLine = AlignSent(srcSent, tgtSent);
    // } 
    // boost::mpi::broadcast<string>(*learningInfo.mpiWorld, alignmentsLine, sentId % learningInfo.mpiWorld->size());
    // if(learningInfo.mpiWorld->rank() == 0) {
      //cerr << "rank #" << learningInfo.mpiWorld->rank() << "sent #" << sentId << " aligned by process #" << sentId % learningInfo.mpiWorld->size() << " as follows: " << alignmentsLine;
      cerr << "sent #" << sentId << " aligned as follows: " << alignmentsLine;
      outputAlignments << alignmentsLine;
    // }
  }
  // if(learningInfo.mpiWorld->rank() == 0) {
    outputAlignments.close();
  // }
}
