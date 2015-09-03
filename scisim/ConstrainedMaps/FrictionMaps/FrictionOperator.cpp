// FrictionOperator.cpp
//
// Breannan Smith
// Last updated: 09/03/2015

#include "FrictionOperator.h"

#include "SCISim/Constraints/Constraint.h"

FrictionOperator::~FrictionOperator()
{}

void FrictionOperator::formSingleSampleGeneralizedFrictionBasisGivenNormalsAndTangents( const unsigned ndofs, const unsigned ncons, const VectorXs& q, const std::vector<std::unique_ptr<Constraint>>& K, const std::vector<std::pair<Vector3s,Vector3s>>& basis_frames, SparseMatrixsc& D )
{
  assert( ncons == K.size() );

  D.resize( ndofs, ncons );

  VectorXi column_nonzeros{ D.cols() };
  std::vector<std::unique_ptr<Constraint>>::const_iterator itr{ K.begin() };
  for( unsigned i = 0; i < ncons; ++i )
  {
    column_nonzeros( i ) = (*itr)->frictionStencilSize();
    ++itr;
  }
  assert( itr == K.end() );
  D.reserve( column_nonzeros );

  itr = K.begin();
  for( unsigned i = 0; i < ncons; ++i )
  {
    (*itr)->computeGeneralizedFrictionGivenTangentSample( q, basis_frames[i].second, i, D );
    ++itr;
  }
  assert( itr == K.end() );

  D.makeCompressed();
}

// TODO: Despecialize from smooth
void FrictionOperator::formGeneralizedSmoothFrictionBasis( const unsigned ndofs, const unsigned ncons, const VectorXs& q, const std::vector<std::unique_ptr<Constraint>>& K, const MatrixXXsc& bases, SparseMatrixsc& D )
{
  assert( ncons == K.size() );

  const unsigned nambientdims{ static_cast<unsigned>( bases.rows() ) };
  const unsigned nsamples{ nambientdims - 1 };

  D.resize( ndofs, nsamples * ncons );

  std::vector<std::unique_ptr<Constraint>>::const_iterator itr{ K.begin() };
  {
    VectorXi column_nonzeros( D.cols() );
    for( unsigned collision_number = 0; collision_number < ncons; ++collision_number )
    {
      for( unsigned sample_number = 0; sample_number < nsamples; ++sample_number )
      {
        assert( nsamples * collision_number + sample_number < column_nonzeros.size() );
        column_nonzeros( nsamples * collision_number + sample_number ) = (*itr)->frictionStencilSize();
      }
      ++itr;
    }
    assert( ( column_nonzeros.array() > 0 ).all() );
    assert( itr == K.end() );
    D.reserve( column_nonzeros );
  }

  itr = K.begin();
  for( unsigned collision_number = 0; collision_number < ncons; ++collision_number )
  {
    for( unsigned sample_number = 0; sample_number < nsamples; ++sample_number )
    {
      const unsigned current_column{ nsamples * collision_number + sample_number };
      const VectorXs current_sample{ bases.col( nambientdims * collision_number + sample_number + 1 ) };
      assert( fabs( current_sample.dot( bases.col( nambientdims * collision_number ) ) ) <= 1.0e-6 );
      (*itr)->computeGeneralizedFrictionGivenTangentSample( q, current_sample, current_column, D );
    }
    ++itr;
  }
  assert( itr == K.end() );

  D.makeCompressed();
}

#include <iostream>

void FrictionOperator::formGeneralizedFrictionBasis( const VectorXs& q, const VectorXs& v, const std::vector<std::unique_ptr<Constraint>>& K, SparseMatrixsc& D, VectorXs& drel )
{
  std::cerr << "Deprecated method FrictionOperator::formGeneralizedFrictionBasis not implemented for " << name() << std::endl;
  std::exit( EXIT_FAILURE );
}