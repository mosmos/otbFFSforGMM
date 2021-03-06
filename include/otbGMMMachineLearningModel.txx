#ifndef __otbGMMMachineLearningModel_txx
#define __otbGMMMachineLearningModel_txx

#include <iostream>
#include <fstream>
#include <math.h>
#include <limits>
#include <vector>
#include <numeric>

#include "otbGMMMachineLearningModel.h"

#include "vnl/vnl_copy.h"
#include "vnl/algo/vnl_symmetric_eigensystem.h"
#include "otbConfusionMatrixCalculator.h"

namespace otb
{

template <class TInputValue, class TTargetValue>
GMMMachineLearningModel<TInputValue,TTargetValue>
::GMMMachineLearningModel():
  m_ClassNb(0),
  m_FeatNb(0),
  m_Tau(0),
  m_ConfidenceIndex(true)
{
}

template <class TInputValue, class TTargetValue>
GMMMachineLearningModel<TInputValue,TTargetValue>
::~GMMMachineLearningModel()
{
}

/***************************************/
/******     Regularization      ********/
/***************************************/

/** Set m_Tau and update m_LambdaQ and m_CstDecision */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::SetTau(RealType tau)
{
  m_Tau = tau;

  // Precompute lambda^(-1/2) * Q, log(det lambda) and -2 log(proportion)
  if (! (m_Q.empty()))
  {
    // Allocation
    RealType lambda;
    m_CstDecision.assign(m_ClassNb,0);
    m_LambdaQ.resize(m_ClassNb, MatrixType(m_FeatNb,m_FeatNb));

    // COULD BE PARALLELIZED (small benefit)
    for (unsigned int i = 0; i < m_ClassNb; ++i)
    {
      for (unsigned int j = 0; j < m_FeatNb; ++j)
      {
        lambda = 1 / sqrt(m_EigenValues[i][j] + m_Tau);
        for (unsigned int k = 0; k < m_FeatNb; ++k)
          m_LambdaQ[i].set_row(j,lambda*m_Q[i].get_column(j));

        m_CstDecision[i] += log(m_EigenValues[i][j] + m_Tau);
      }

      m_CstDecision[i] += -2*log(m_Proportion[i]);
    }
  }
}

/** Make a gridsearch with cross-validation to select the appropriate tau */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::TrainTau(std::vector<RealType> tauGrid, int nfold, const std::string & criterion, int seed)
{
  // Creation of submodel for cross-validation
  typedef typename ClassSampleType::Pointer ClassSamplePointer;
  std::vector<GMMMachineLearningModel<TInputValue, TTargetValue>::Pointer > submodelCv(nfold);
  std::vector<std::vector<ClassSamplePointer> > folds(nfold);

  // Allocation
  typedef typename InputListSampleType::InstanceIdentifier  InstanceIdentifier;
  typedef itk::Statistics::CovarianceSampleFilter< itk::Statistics::Subsample< InputListSampleType > > CovarianceEstimatorType;
  for (int j = 0; j < nfold; ++j)
    submodelCv[j] = GMMMachineLearningModel<TInputValue, TTargetValue>::New();
  typename CovarianceEstimatorType::Pointer covarianceEstimator = CovarianceEstimatorType::New();
  VectorType meanFold;
  MatrixType covarianceFold, adjustedMean;
  unsigned nbSplFold;

  for (unsigned int i = 0; i < m_ClassNb; ++i)
  {
    // Get all sample ids from ith class
    std::vector<InstanceIdentifier> indices(m_NbSpl[i]);
    for (unsigned j=0; j<m_NbSpl[i]; ++j)
      indices[j] = (m_ClassSamples[i])->GetInstanceIdentifier(j);

    // Shuffle ids
    std::srand( unsigned( seed ) );
    std::random_shuffle( indices.begin(), indices.end() );

    // Compute number of samples by fold
    nbSplFold = m_NbSpl[i]/nfold;

    for (int j = 0; j < nfold; ++j)
    {
      // Add subpart of id to fold
      if (j==nfold-1)
      {
        // Last fold get the remaining samples
        folds[j].push_back( ClassSampleType::New() );
        folds[j][folds[j].size()-1]->SetSample( Superclass::GetInputListSample() );
        for (unsigned k = j*nbSplFold; k < m_NbSpl[i]; ++k)
          folds[j][folds[j].size()-1]->AddInstance( indices[k] );
        submodelCv[j]->AddNbSpl(m_NbSpl[i] - j*nbSplFold);
        nbSplFold = m_NbSpl[i] - j*nbSplFold;
      }
      else
      {
        folds[j].push_back( ClassSampleType::New() );
        folds[j][folds[j].size()-1]->SetSample( Superclass::GetInputListSample() );
        for (unsigned k = j*nbSplFold; k < (j+1)*nbSplFold; ++k)
          folds[j][folds[j].size()-1]->AddInstance( indices[k] );
        submodelCv[j]->AddNbSpl(nbSplFold);
      }

      // Update model for each fold
      submodelCv[j]->SetMapOfClasses(m_MapOfClasses);
      submodelCv[j]->SetMapOfIndices(m_MapOfIndices);
      submodelCv[j]->SetClassNb(m_ClassNb);
      submodelCv[j]->SetFeatNb(m_FeatNb);

      covarianceEstimator->SetInput( folds[j][i] );
      covarianceEstimator->Update();

      covarianceFold = covarianceEstimator->GetCovarianceMatrix().GetVnlMatrix();
      meanFold       = VectorType(covarianceEstimator->GetMean().GetDataPointer(),m_FeatNb);

      submodelCv[j]->AddMean( (1/(RealType)(m_NbSpl[i] - nbSplFold)) * ((RealType) m_NbSpl[i] * m_Means[i] - (RealType) nbSplFold * meanFold) );
      adjustedMean = MatrixType((m_Means[i]-meanFold).data_block(), m_FeatNb, 1); // convert in matrix
      submodelCv[j]->AddCovMatrix( (1/(RealType)(m_NbSpl[i]-nbSplFold-1)) * ( ((RealType)m_NbSpl[i]-1)*m_Covariances[i] - ((RealType)nbSplFold-1)*covarianceFold - (RealType)m_NbSpl[i]*(RealType)nbSplFold/(RealType)(m_NbSpl[i]-nbSplFold) * adjustedMean * adjustedMean.transpose() ) ); // convert all unsigned in realType
    }
  }

  for (int i = 0; i < nfold; ++i)
  {
    submodelCv[i]->UpdateProportion();
    submodelCv[i]->UpdateDecomposition();
  }

  // Perform grid-search
  typedef ConfusionMatrixCalculator< TargetListSampleType, TargetListSampleType > ConfusionMatrixType;
  TargetSampleType res;
  m_RateGridsearch.resize(tauGrid.size());
  std::vector<RealType>::iterator outGridsearchIt = m_RateGridsearch.begin();

  for (std::vector<RealType>::iterator tauIt = tauGrid.begin(); tauIt != tauGrid.end(); ++tauIt, ++outGridsearchIt)
  {
    typename TargetListSampleType::Pointer TargetListSample    = TargetListSampleType::New();
    typename TargetListSampleType::Pointer RefTargetListSample = TargetListSampleType::New();
    typename ConfusionMatrixType::Pointer confM = ConfusionMatrixType::New();

    // COULD BE PARALLELIZED and could also use filter to predict...
    // Classify with all submodels for a given tau
    for (int i = 0; i < nfold; ++i)
    {
      submodelCv[i]->SetTau(*tauIt);

      for (unsigned int c = 0; c < m_ClassNb; ++c)
      {
        nbSplFold = m_NbSpl[c]/nfold;

        if (i==nfold-1)
        {
          for (unsigned s = 0; s < (m_NbSpl[c] - i*nbSplFold); ++s)
          {
            res = submodelCv[i]->DoPredict(folds[i][c]->GetMeasurementVectorByIndex(s));
            TargetListSample->PushBack(res);
            res[0] = m_MapOfIndices.at(c);
            RefTargetListSample->PushBack(res);
          }
        }
        else
        {
          for (unsigned s = 0; s < nbSplFold; ++s)
          {
            res = submodelCv[i]->DoPredict(folds[i][c]->GetMeasurementVectorByIndex(s));
            TargetListSample->PushBack(res);
            res[0] = m_MapOfIndices.at(c);
            RefTargetListSample->PushBack(res);
          }
        }
      }
    }

    // Evaluate classification rate
    confM->SetReferenceLabels(RefTargetListSample);
    confM->SetProducedLabels(TargetListSample);
    confM->Compute();

    if (criterion.compare("accuracy") == 0)
    {
      *outGridsearchIt = (RealType) confM->GetOverallAccuracy();
    }
    else if (criterion.compare("kappa") == 0)
    {
      *outGridsearchIt = (RealType) confM->GetKappaIndex();
    }
    else if (criterion.compare("f1mean") == 0)
    {
      typename ConfusionMatrixType::MeasurementType Fscores = confM->GetFScores();
      RealType meanFscores = 0;
      for (unsigned i = 0; i < Fscores.Size(); ++i)
        meanFscores += (RealType) Fscores[i];
      *outGridsearchIt = meanFscores/m_ClassNb;
    }
  }

  // Keep best tau
  int argmax = std::distance(m_RateGridsearch.begin(), std::max_element(m_RateGridsearch.begin(), m_RateGridsearch.end()));
  this->SetTau(tauGrid[argmax]);
}

/***************************************/
/**********     Tools      *************/
/***************************************/

/** Update the proportion of samples */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::UpdateProportion()
{
  unsigned totalNb = 0;
  for (unsigned int i = 0; i < m_ClassNb; ++i)
    totalNb += m_NbSpl[i];

  m_Proportion.resize(m_ClassNb);
  for (unsigned int i = 0; i < m_ClassNb; ++i)
    m_Proportion[i] = (double) m_NbSpl[i] / (double) totalNb;
}

/** Update the proportion of samples */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::UpdateDecomposition()
{
  m_Q.resize(m_ClassNb,MatrixType(m_FeatNb,m_FeatNb));
  m_EigenValues.resize(m_ClassNb,VectorType(m_FeatNb));

  for (unsigned int i = 0; i < m_ClassNb; ++i)
  {
    // Decompose covariance matrix in eigenvalues/eigenvectors
    Decomposition(m_Covariances[i], m_Q[i], m_EigenValues[i]);
  }
}

/** Functions to update model (mean) */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::AddMean(VectorType vector)
{
  m_Means.push_back(vector);
}

/** Functions to update model (covariance matrix) */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::AddCovMatrix(MatrixType covMatrix)
{
  m_Covariances.push_back(covMatrix);
}

/** Functions to update model (spl nb) */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::AddNbSpl(unsigned long n)
{
  m_NbSpl.push_back(n);
}

/** Compute decomposition in eigenvalues and eigenvectors of a symmetric matrix */
template <class TInputValue, class TTargetValue>
void GMMMachineLearningModel<TInputValue,TTargetValue>
::Decomposition(MatrixType &inputMatrix, MatrixType &outputMatrix, VectorType &eigenValues)
{
  vnl_symmetric_eigensystem_compute( inputMatrix, outputMatrix, eigenValues );

  // Replace eigenvalues lower than min precision by min precision
  for (VectorType::iterator it = eigenValues.begin(); it != eigenValues.end(); ++it)
  {
    if (*it < std::numeric_limits<RealType>::epsilon())
      *it = std::numeric_limits<RealType>::epsilon();
  }
}

/***************************************/
/*******     Classification    *********/
/***************************************/

/** Train the machine learning model */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::Train()
{
  Superclass::m_ConfidenceIndex = true;

  // Get pointer to samples and labels
  typename InputListSampleType::Pointer samples = Superclass::GetInputListSample();
  typename TargetListSampleType::Pointer labels = Superclass::GetTargetListSample();

  // Declare iterator for samples and labels
  typename TargetListSampleType::ConstIterator refIterator  = labels->Begin();
  typename InputListSampleType::ConstIterator inputIterator = samples->Begin();

  // Get number of samples
  float sampleNb = (float) labels->Size();

  // Get number of features
  m_FeatNb = samples->GetMeasurementVectorSize();

  // Get number of classes and map indice with label
  TargetValueType currentLabel;
  while (refIterator != labels->End())
  {
    currentLabel = refIterator.GetMeasurementVector()[0];
    if (m_MapOfClasses.find(currentLabel) == m_MapOfClasses.end())
    {
      m_MapOfClasses[currentLabel] = m_ClassNb;
      m_MapOfIndices[m_ClassNb]    = currentLabel;
      ++m_ClassNb;
    }
    ++refIterator;
  }

  // Create one subsample set for each class
  m_ClassSamples.resize(m_ClassNb);
  for ( unsigned int i = 0; i < m_ClassNb; ++i )
  {
    m_ClassSamples[i] = ClassSampleType::New();
    m_ClassSamples[i]->SetSample( samples );
  }
  inputIterator = samples->Begin();
  refIterator   = labels->Begin();
  while (inputIterator != samples->End())
  {
    currentLabel = refIterator.GetMeasurementVector()[0];
    m_ClassSamples[m_MapOfClasses[currentLabel]]->AddInstance( inputIterator.GetInstanceIdentifier() );
    ++inputIterator;
    ++refIterator;
  }

  // Allocate all members
  m_NbSpl.resize(m_ClassNb);
  m_Proportion.resize(m_ClassNb);
  m_Covariances.resize(m_ClassNb,MatrixType(m_FeatNb,m_FeatNb));
  m_Means.resize(m_ClassNb,VectorType(m_FeatNb));

  m_Q.resize(m_ClassNb,MatrixType(m_FeatNb,m_FeatNb));
  m_EigenValues.resize(m_ClassNb,VectorType(m_FeatNb));

  m_CstDecision.resize(m_ClassNb,0);
  m_LambdaQ.resize(m_ClassNb, MatrixType(m_FeatNb,m_FeatNb));

  RealType lambda;
  typedef itk::Statistics::CovarianceSampleFilter< itk::Statistics::Subsample< InputListSampleType > > CovarianceEstimatorType;
  typename CovarianceEstimatorType::Pointer covarianceEstimator = CovarianceEstimatorType::New();
  // COULD BE PARALLELIZED
  for ( unsigned int i = 0; i < m_ClassNb; ++i )
  {
    // Estimate covariance matrices, mean vectors and proportions
    m_NbSpl[i] = m_ClassSamples[i]->Size();
    m_Proportion[i] = (float) m_NbSpl[i] / sampleNb;

    covarianceEstimator->SetInput( m_ClassSamples[i] );
    covarianceEstimator->Update();

    m_Covariances[i] = covarianceEstimator->GetCovarianceMatrix().GetVnlMatrix();
    m_Means[i] = VectorType(covarianceEstimator->GetMean().GetDataPointer(),m_FeatNb);

    // Decompose covariance matrix in eigenvalues/eigenvectors
    Decomposition(m_Covariances[i], m_Q[i], m_EigenValues[i]);

    // Precompute lambda^(-1/2) * Q and log(det lambda)
    for (unsigned j = 0; j < m_FeatNb; ++j)
    {
      lambda = 1 / sqrt(m_EigenValues[i][j] + m_Tau);
      // Transposition and row multiplication at the same time
      m_LambdaQ[i].set_row(j,lambda*m_Q[i].get_column(j));

      m_CstDecision[i] += log(m_EigenValues[i][j] + m_Tau);
    }

    m_CstDecision[i] += -2*log(m_Proportion[i]);
  }
}

/** Predict values using the model */
template <class TInputValue, class TTargetValue>
typename GMMMachineLearningModel<TInputValue,TTargetValue>
::TargetSampleType
GMMMachineLearningModel<TInputValue,TTargetValue>
::DoPredict(const InputSampleType & rawInput, ConfidenceValueType *quality) const
{
  // Convert input data
  VectorType input(m_FeatNb);
  vnl_copy(vnl_vector<InputValueType>(rawInput.GetDataPointer(), m_FeatNb),input);

  // Compute decision function
  std::vector<RealType> decisionFct(m_CstDecision);
  VectorType lambdaQInputC(m_FeatNb);
  VectorType input_c(m_FeatNb);
  for (unsigned i = 0; i < m_ClassNb; ++i)
  {
    input_c = input - m_Means[i];
    lambdaQInputC = m_LambdaQ[i] * input_c;

    // Add sum of squared elements
    decisionFct[i] += lambdaQInputC.squared_magnitude();
  }

  int argmin = std::distance(decisionFct.begin(), std::min_element(decisionFct.begin(), decisionFct.end()));

  TargetSampleType res;
  res[0] = m_MapOfIndices.at(argmin);

  // Compute confidence (optional)
  if (quality != NULL)
  {
    if (!this->HasConfidenceIndex())
    {
      itkExceptionMacro("Confidence index not available for this classifier !");
    }
    else
    {
      RealType prob = 0;
      for (std::vector<RealType>::iterator decisionIt = decisionFct.begin(); decisionIt != decisionFct.end(); ++decisionIt)
        prob += exp(-0.5*( *decisionIt - decisionFct[argmin] ));
      *quality = (ConfidenceValueType) ( 1 / prob);
    }
  }

  return res;
}

/***************************************/
/*********     Read/Write    ***********/
/***************************************/

/** Save the model to file */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::Save(const std::string & filename, const std::string & name)
{
  // create and open a character archive for output
  std::ofstream ofs(filename.c_str(), std::ios::out);

  // Store header
  ofs << "GMMmodel"<< std::endl;

  // Store single value data
  ofs << "classnb: " << m_ClassNb << std::endl;
  ofs << "featnb: " << m_FeatNb << std::endl;
  ofs << "tau: " << m_Tau << std::endl;
  ofs << "featNbForDecomp: " << m_EigenValues[0].size() << std::endl;

  // Store label mapping (only one way)
  ofs << "LabelMapping:" << std::endl;
  typedef typename std::map<TargetValueType, int>::const_iterator MapIterator;
  for (MapIterator classMapIter = m_MapOfClasses.begin(); classMapIter != m_MapOfClasses.end(); classMapIter++)
    ofs << classMapIter->first << " " << classMapIter->second << " ";
  ofs << std::endl;

  // Store vector of nb of samples
  ofs << "SamplesNb:" << std::endl;
  for (std::vector<unsigned>::iterator it = m_NbSpl.begin(); it != m_NbSpl.end(); ++it)
    ofs << *it << " ";
  ofs << std::endl;

  // Set writing precision (need c++11 to not hardcode value of double precision)
  // ofs.precision(std::numeric_limits<double>::max_digits10);
  ofs.precision(17);

  // Store vector of proportion of samples
  ofs << "Proportion:" << std::endl;
  for (std::vector<double>::iterator it = m_Proportion.begin(); it != m_Proportion.end(); ++it)
    ofs << *it << " ";
  ofs << std::endl;

  // Store vector of mean vector (one by line)
  ofs << "MeanVector:" << std::endl;
  for (std::vector<VectorType>::iterator classIt = m_Means.begin(); classIt != m_Means.end(); ++classIt)
  {
    for (VectorType::iterator featIt = (*classIt).begin(); featIt != (*classIt).end(); ++featIt)
      ofs << *featIt << " ";

    ofs << std::endl;
  }

  // Store vector of covariance matrices (one by line)
  ofs << "CovarianceMatrices:" << std::endl;
  for (std::vector<MatrixType>::iterator classIt = m_Covariances.begin(); classIt != m_Covariances.end(); ++classIt)
  {
    for (unsigned j = 0; j < m_FeatNb; ++j)
    {
      for (unsigned k = 0; k < m_FeatNb; ++k)
        ofs << (*classIt)(j,k) << " ";
    }
    ofs << std::endl;
  }

  // Store vector of eigenvalues vector (one by line)
  ofs << "Eigenvalues:" << std::endl;
  for (std::vector<VectorType>::iterator classIt = m_EigenValues.begin(); classIt != m_EigenValues.end(); ++classIt)
  {
    for (VectorType::iterator featIt = (*classIt).begin(); featIt != (*classIt).end(); ++featIt)
      ofs << *featIt << " ";

    ofs << std::endl;
  }

  // Store vector of eigenvectors matrices (one by line)
  ofs << "Eigenvectors:" << std::endl;
  for (std::vector<MatrixType>::iterator classIt = m_Q.begin(); classIt != m_Q.end(); ++classIt)
  {
    for (unsigned int j = 0; j < (*classIt).rows(); ++j)
    {
      for (unsigned int k = 0; k < (*classIt).cols(); ++k)
        ofs << (*classIt)(j,k) << " ";
    }
    ofs << std::endl;
  }

  // Store vector of eigenvalues^(-1/2) * Q.T matrices (one by line)
  ofs << "LambdaQ:" << std::endl;
  for (std::vector<MatrixType>::iterator classIt = m_LambdaQ.begin(); classIt != m_LambdaQ.end(); ++classIt)
  {
    for (unsigned int j = 0; j < (*classIt).rows(); ++j)
    {
      for (unsigned int k = 0; k < (*classIt).cols(); ++k)
        ofs << (*classIt)(j,k) << " ";
    }
    ofs << std::endl;
  }

  // Store vector of scalar (logdet cov - 2*log proportion)
  ofs << "CstDecision:" << std::endl;
  for (std::vector<RealType>::iterator classIt = m_CstDecision.begin(); classIt != m_CstDecision.end(); ++classIt)
    ofs << *classIt << " ";
  ofs << std::endl;

  ofs.close();
}

/** Load the model from file */
template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::Load(const std::string & filename, const std::string & name)
{
  Superclass::m_ConfidenceIndex = true;

  std::ifstream ifs(filename.c_str(), std::ios::in);

  std::string header, dump;
  int decompVarNb;
  // Store header
  ifs >> header;

  // Load single value data
  ifs >> dump;
  ifs >> m_ClassNb;
  ifs >> dump;
  ifs >> m_FeatNb;
  ifs >> dump;
  ifs >> m_Tau;
  ifs >> dump;
  ifs >> decompVarNb;

  // Allocation
  m_NbSpl.resize(m_ClassNb);
  m_Proportion.resize(m_ClassNb);
  m_Means.resize(m_ClassNb,VectorType(m_FeatNb));
  m_Covariances.resize(m_ClassNb,MatrixType(m_FeatNb,m_FeatNb));
  m_EigenValues.resize(m_ClassNb,VectorType(decompVarNb));
  m_Q.resize(m_ClassNb,MatrixType(decompVarNb,decompVarNb));
  m_LambdaQ.resize(m_ClassNb,MatrixType(decompVarNb,decompVarNb));
  m_CstDecision.resize(m_ClassNb);

  // Load label mapping (only one way)
  ifs >> dump;
  TargetValueType lab;
  int idLab;
  for (unsigned i = 0; i < m_ClassNb; ++i)
  {
    ifs >> lab;
    ifs >> idLab;
    m_MapOfClasses[lab]   = idLab;
    m_MapOfIndices[idLab] = lab;
  }

  // Load vector of nb of samples
  ifs >> dump;
  for (std::vector<unsigned>::iterator it = m_NbSpl.begin(); it != m_NbSpl.end(); ++it)
    ifs >> *it;

  // Load vector of proportion of samples
  ifs >> dump;
  for (std::vector<double>::iterator it = m_Proportion.begin(); it != m_Proportion.end(); ++it)
    ifs >> *it;

  // Load vector of mean vector (one by line)
  ifs >> dump;
  for (std::vector<VectorType>::iterator classIt = m_Means.begin(); classIt != m_Means.end(); ++classIt)
    for (VectorType::iterator featIt = (*classIt).begin(); featIt != (*classIt).end(); ++featIt)
      ifs >> *featIt;

  // Load vector of covariance matrices (one by line)
  ifs >> dump;
  for (std::vector<MatrixType>::iterator classIt = m_Covariances.begin(); classIt != m_Covariances.end(); ++classIt)
    for (unsigned j = 0; j < m_FeatNb; ++j)
      for (unsigned k = 0; k < m_FeatNb; ++k)
        ifs >> (*classIt)(j,k);

  // Load vector of eigenvalues vector (one by line)
  ifs >> dump;
  for (std::vector<VectorType>::iterator classIt = m_EigenValues.begin(); classIt != m_EigenValues.end(); ++classIt)
    for (VectorType::iterator featIt = (*classIt).begin(); featIt != (*classIt).end(); ++featIt)
      ifs >> *featIt;

  // Load vector of eigenvectors matrices (one by line)
  ifs >> dump;
  for (std::vector<MatrixType>::iterator classIt = m_Q.begin(); classIt != m_Q.end(); ++classIt)
    for (int j = 0; j < decompVarNb; ++j)
      for (int k = 0; k < decompVarNb; ++k)
        ifs >> (*classIt)(j,k);

  // Load vector of eigenvalues^(-1/2) * Q.T matrices (one by line)
  ifs >> dump;
  for (std::vector<MatrixType>::iterator classIt = m_LambdaQ.begin(); classIt != m_LambdaQ.end(); ++classIt)
    for (int j = 0; j < decompVarNb; ++j)
      for (int k = 0; k < decompVarNb; ++k)
        ifs >> (*classIt)(j,k);

  // Load vector of scalar (logdet cov - 2*log proportion)
  ifs >> dump;
  for (std::vector<RealType>::iterator it = m_CstDecision.begin(); it != m_CstDecision.end(); ++it)
    ifs >>*it;

  ifs.close();
}

/** Is the input model file readable and compatible with the corresponding classifier ? */
template <class TInputValue, class TTargetValue>
bool
GMMMachineLearningModel<TInputValue,TTargetValue>
::CanReadFile(const std::string & file)
{
  std::ifstream ifs(file.c_str(), std::ios::in);

  if(!ifs)
  {
    std::cerr<<"Could not read file "<<file<<std::endl;
    return false;
  }

  std::string header;
  // Store header
  ifs >> header;
  ifs.close();

  if (header.compare("GMM model"))
    return true;

  return false;
}

/** Is the input model file writable and compatible with the corresponding classifier ? */
template <class TInputValue, class TTargetValue>
bool
GMMMachineLearningModel<TInputValue,TTargetValue>
::CanWriteFile(const std::string & itkNotUsed(file))
{
  return false;
}

template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::PrintSelf(std::ostream& os, itk::Indent indent) const
{
  // Call superclass implementation
  Superclass::PrintSelf(os,indent);
}

/***************************************/
/********       Accessors       ********/
/***************************************/

template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::SetMapOfClasses(const std::map<TargetValueType, int>& mapOfClasses)
{
  m_MapOfClasses = mapOfClasses;
}

template <class TInputValue, class TTargetValue>
void
GMMMachineLearningModel<TInputValue,TTargetValue>
::SetMapOfIndices(const std::map<int, TargetValueType>& mapOfIndices)
{
 m_MapOfIndices = mapOfIndices;
}

} //end namespace otb

#endif
