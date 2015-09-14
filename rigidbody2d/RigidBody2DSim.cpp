// RigidBody2DSim.cpp
//
// Breannan Smith
// Last updated: 09/10/2015

#include "RigidBody2DSim.h"

#include "SCISim/UnconstrainedMaps/UnconstrainedMap.h"
#include "SCISim/Math/MathUtilities.h"
#include "SCISim/ConstrainedMaps/ImpactMaps/ImpactMap.h"
#include "SCISim/ScriptingCallback.h"
#include "SCISim/ConstrainedMaps/ImpactFrictionMap.h"
#include "SCISim/Utilities.h"
#include "SCISim/HDF5File.h"

#include "CircleGeometry.h"
#include "StaticPlaneCircleConstraint.h"
#include "CircleCircleConstraint.h"
#include "TeleportedCircleCircleConstraint.h"
#include "KinematicKickCircleCircleConstraint.h"
#include "SpatialGrid.h"
#include "StateOutput.h"
#include "PythonScripting.h"

#include <iostream>

RigidBody2DSim::RigidBody2DSim()
: m_state()
{}

RigidBody2DSim::RigidBody2DSim( const RigidBody2DSim& other )
: m_state( other.m_state )
{}

RigidBody2DSim::RigidBody2DSim( const RigidBody2DState& state )
: m_state( state )
{}

RigidBody2DSim& RigidBody2DSim::operator=( RigidBody2DSim other )
{
  swap( *this, other );
  return *this;
}

void swap( RigidBody2DSim& first, RigidBody2DSim& second )
{
  using std::swap;
  swap( first.m_state, second.m_state );
}

RigidBody2DState& RigidBody2DSim::state()
{
  return m_state;
}

const RigidBody2DState& RigidBody2DSim::state() const
{
  return m_state;
}

scalar RigidBody2DSim::computeKineticEnergy() const
{
  return 0.5 * m_state.v().dot( m_state.M() * m_state.v() );
}

scalar RigidBody2DSim::computePotentialEnergy() const
{
  scalar U{ 0.0 };
  const std::vector<std::unique_ptr<RigidBody2DForce>>& forces = m_state.forces();
  for( const std::unique_ptr<RigidBody2DForce>& force : forces )
  {
    U += force->computePotential( m_state.q(), m_state.M() );
  }
  return U;
}

scalar RigidBody2DSim::computeTotalEnergy() const
{
  return computeKineticEnergy() + computePotentialEnergy();
}

Vector2s RigidBody2DSim::computeTotalMomentum() const
{
  VectorXs p;
  computeMomentum( m_state.v(), p );
  assert( p.size() == 2 );
  return p;
}

scalar RigidBody2DSim::computeTotalAngularMomentum() const
{
  VectorXs L;
  computeAngularMomentum( m_state.v(), L );
  assert( L.size() == 1 );
  return L( 0 );
}

int RigidBody2DSim::nqdofs() const
{
  return m_state.q().size();
}

int RigidBody2DSim::nvdofs() const
{
  return m_state.v().size();
}

unsigned RigidBody2DSim::numVelDoFsPerBody() const
{
  return 3;
}

unsigned RigidBody2DSim::ambientSpaceDimensions() const
{
  return 2;
}

bool RigidBody2DSim::isKinematicallyScripted( const int i ) const
{
  return false;
}

void RigidBody2DSim::computeForce( const VectorXs& q, const VectorXs& v, const scalar& t, VectorXs& F )
{
  assert( q.size() % 3 == 0 ); assert( v.size() == q.size() ); assert( v.size() == F.size() );
  F.setZero();
  const std::vector<std::unique_ptr<RigidBody2DForce>>& forces{ m_state.forces() };
  for( const std::unique_ptr<RigidBody2DForce>& force : forces )
  {
    force->computeForce( q, v, m_state.M(), F );
  }
}

const SparseMatrixsc& RigidBody2DSim::M() const
{
  return m_state.M();
}

const SparseMatrixsc& RigidBody2DSim::Minv() const
{
  return m_state.Minv();
}

const SparseMatrixsc& RigidBody2DSim::M0() const
{
  // Mass matrix is invaraint to configuration for this system
  return m_state.M();
}

const SparseMatrixsc& RigidBody2DSim::Minv0() const
{
  // Mass matrix is invaraint to configuration for this system
  return m_state.Minv();
}

void RigidBody2DSim::computeMomentum( const VectorXs& v, VectorXs& p ) const
{
  p = Vector2s::Zero();
  assert( m_state.q().size() % 3 == 0 );
  const unsigned nbodies{ static_cast<unsigned>( m_state.q().size() / 3 ) };
  for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
  {
    p += m_state.m( bdy_idx ) * v.segment<2>( 3 * bdy_idx );
  }
}

void RigidBody2DSim::computeAngularMomentum( const VectorXs& v, VectorXs& L ) const
{
  L = VectorXs::Zero( 1 );
  assert( m_state.q().size() % 3 == 0 );
  const unsigned nbodies{ static_cast<unsigned>( m_state.q().size() / 3 ) };
  // Contribution from center of mass
  for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
  {
    L(0) += m_state.m( bdy_idx ) * MathUtilities::cross( m_state.q().segment<2>( 3 * bdy_idx ), v.segment<2>( 3 * bdy_idx ) );
  }
  // Contribution from rotation about center of mass
  for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
  {
    L(0) += m_state.I( bdy_idx ) * v( 3 * bdy_idx + 2 );
  }
}

// TODO: Replace nested switch statements with jump table or something similar
static void dispatchNarrowPhaseCollision( const unsigned idx0, const unsigned idx1, const std::unique_ptr<RigidBody2DGeometry>& geo0, const std::unique_ptr<RigidBody2DGeometry>& geo1, const VectorXs& q0, const VectorXs& q1, std::vector<std::unique_ptr<Constraint>>& active_set )
{
  assert( q0.size() % 3 == 0 ); assert( q0.size() == q1.size() );

  switch( geo0->type() )
  {
    case RigidBody2DGeometry::CIRCLE:
    {
      const CircleGeometry& circle_geo0{ sd_cast<CircleGeometry&>( *geo0 ) };
      switch( geo1->type() )
      {
        case RigidBody2DGeometry::CIRCLE:
        {
          const CircleGeometry& circle_geo1{ sd_cast<CircleGeometry&>( *geo1 ) };
          if( CircleCircleConstraint::isActive( q1.segment<2>( 3 * idx0 ), q1.segment<2>( 3 * idx1 ), circle_geo0.r(), circle_geo1.r() ) )
          {
            // Creation of constraints at q0 to preserve angular momentum
            const Vector2s n{ ( q0.segment<2>( 3 * idx0 ) - q0.segment<2>( 3 * idx1 ) ).normalized() };
            const Vector2s p{ q0.segment<2>( 3 * idx0 ) + ( circle_geo0.r() / ( circle_geo0.r() + circle_geo1.r() ) ) * ( q0.segment<2>( 3 * idx1 ) - q0.segment<2>( 3 * idx0 ) ) };
            active_set.emplace_back( new CircleCircleConstraint{ idx0, idx1, n, p, circle_geo0.r(), circle_geo1.r() } );
          }
          break;
        }
      }
      break;
    }
  }
}

static bool collisionIsActive( const Vector2s& x0, const scalar& theta0, const std::unique_ptr<RigidBody2DGeometry>& geo0, const Vector2s& x1, const scalar& theta1, const std::unique_ptr<RigidBody2DGeometry>& geo1 )
{
  switch( geo0->type() )
  {
    case RigidBody2DGeometry::CIRCLE:
    {
      const CircleGeometry& circle_geo0{ sd_cast<CircleGeometry&>( *geo0 ) };
      switch( geo1->type() )
      {
        case RigidBody2DGeometry::CIRCLE:
        {
          const CircleGeometry& circle_geo1{ sd_cast<CircleGeometry&>( *geo1 ) };
          if( CircleCircleConstraint::isActive( x0, x1, circle_geo0.r(), circle_geo1.r() ) )
          {
            return true;
          }
          else
          {
            return false;
          }
        }
      }
    }
    // GCC and Intel don't realize that we've exhausted all cases and complain about no return here.
#ifndef CMAKE_DETECTED_CLANG_COMPILER
    default:
    {
      std::cerr << "Invalid geometry type in RigidBody2DSim::collisionIsActive, this is a bug." << std::endl;
      std::exit( EXIT_FAILURE );
    }
#endif
  }
}

static bool collisionIsActive( const unsigned idx0, const unsigned idx1, const std::unique_ptr<RigidBody2DGeometry>& geo0, const std::unique_ptr<RigidBody2DGeometry>& geo1, const VectorXs& q )
{
  assert( q.size() % 3 == 0 );

  const Vector2s x0{ q.segment<2>( 3 * idx0 ) };
  const scalar theta0{ q( 3 * idx0 + 2 ) };
  const Vector2s x1{ q.segment<2>( 3 * idx1 ) };
  const scalar theta1{ q( 3 * idx1 + 2 ) };

  return collisionIsActive( x0, theta0, geo0, x1, theta1, geo1 );
}

bool RigidBody2DSim::teleportedCollisionIsActive( const TeleportedCollision& teleported_collision, const std::unique_ptr<RigidBody2DGeometry>& geo0, const std::unique_ptr<RigidBody2DGeometry>& geo1, const VectorXs& q ) const
{
  assert( q.size() % 3 == 0 );

  // Get the center of mass of each body after the teleportation for the end of the step
  Vector2s x0;
  Vector2s x1;
  getTeleportedCollisionCenters( q, teleported_collision, x0, x1 );

  const scalar theta0{ q( 3 * teleported_collision.bodyIndex0() + 2 ) };
  const scalar theta1{ q( 3 * teleported_collision.bodyIndex1() + 2 ) };

  return collisionIsActive( x0, theta0, geo0, x1, theta1, geo1 );
}

void RigidBody2DSim::getTeleportedCollisionCenter( const unsigned portal_index, const bool portal_plane, Vector2s& x ) const
{
  // TODO: Move this if statement into the portal class
  if( portal_plane == 0 )
  {
    m_state.planarPortals()[ portal_index ].teleportPointThroughPlaneA( x, x );
  }
  else
  {
    m_state.planarPortals()[ portal_index ].teleportPointThroughPlaneB( x, x );
  }
}

void RigidBody2DSim::getTeleportedCollisionCenters( const VectorXs& q, const TeleportedCollision& teleported_collision, Vector2s& x0, Vector2s& x1 ) const
{
  assert( q.size() % 3 == 0 );

  #ifndef NDEBUG
  const unsigned nbodies{ static_cast<unsigned>( q.size() / 3 ) };
  #endif

  // Indices of the colliding bodies
  const unsigned idx0{ teleported_collision.bodyIndex0() };
  assert( idx0 < nbodies );
  const unsigned idx1{ teleported_collision.bodyIndex1() };
  assert( idx1 < nbodies );

  // Indices of the portals
  const unsigned prtl_idx0{ teleported_collision.portalIndex0() };
  assert( prtl_idx0 < m_state.planarPortals().size() || prtl_idx0 == std::numeric_limits<unsigned>::max() );
  const unsigned prtl_idx1{ teleported_collision.portalIndex1() };
  assert( prtl_idx1 < m_state.planarPortals().size() || prtl_idx1 == std::numeric_limits<unsigned>::max() );

  // If the first object was teleported
  assert( 3 * idx0 + 1 < q.size() );
  x0 = q.segment<2>( 3 * idx0 );
  if( prtl_idx0 != std::numeric_limits<unsigned>::max() )
  {
    getTeleportedCollisionCenter( prtl_idx0, teleported_collision.plane0(), x0 );
  }

  // If the second object was teleported
  assert( 3 * idx1 + 1 < q.size() );
  x1 = q.segment<2>( 3 * idx1 );
  if( prtl_idx1 != std::numeric_limits<unsigned>::max() )
  {
    getTeleportedCollisionCenter( prtl_idx1, teleported_collision.plane1(), x1 );
  }
}

void RigidBody2DSim::dispatchTeleportedNarrowPhaseCollision( const TeleportedCollision& teleported_collision, const std::unique_ptr<RigidBody2DGeometry>& geo0, const std::unique_ptr<RigidBody2DGeometry>& geo1, const VectorXs& q0, const VectorXs& q1, std::vector<std::unique_ptr<Constraint>>& active_set ) const
{
  assert( q0.size() % 3 == 0 ); assert( q0.size() == q1.size() );

  // Get the center of mass of each body after the teleportation for the start and end of the step
  Vector2s x0_t0;
  Vector2s x1_t0;
  getTeleportedCollisionCenters( q0, teleported_collision, x0_t0, x1_t0 );
  const Vector2s delta0_t0{ x0_t0 - q0.segment<2>( 3 * teleported_collision.bodyIndex0() ) };
  const Vector2s delta1_t0{ x1_t0 - q0.segment<2>( 3 * teleported_collision.bodyIndex1() ) };

  Vector2s x0_t1;
  Vector2s x1_t1;
  getTeleportedCollisionCenters( q1, teleported_collision, x0_t1, x1_t1 );
  // TODO: If Lees-Edwards conditions are updated to have different locations at start and end of step
  //       (instead of same at each, as now) these tests will no longer hold
  #ifndef NDEBUG
  {
    const Vector2s delta0_t1{ x0_t1 - q1.segment<2>( 3 * teleported_collision.bodyIndex0() ) };
    assert( ( delta0_t0 - delta0_t1 ).lpNorm<Eigen::Infinity>() <= 1.0e-6 );
    const Vector2s delta1_t1{ x1_t1 - q1.segment<2>( 3 * teleported_collision.bodyIndex1() ) };
    assert( ( delta1_t0 - delta1_t1 ).lpNorm<Eigen::Infinity>() <= 1.0e-6 );
  }
  #endif

  // At least one of the bodies must have been teleported
  assert( teleported_collision.portalIndex0() != std::numeric_limits<unsigned>::max() || teleported_collision.portalIndex1() != std::numeric_limits<unsigned>::max() );

  // Determine whether the first body was teleported
  bool portal0_is_lees_edwards{ false };
  #ifndef NDEBUG
  bool first_was_teleported{ false };
  #endif
  if( teleported_collision.portalIndex0() != std::numeric_limits<unsigned>::max() )
  {
    #ifndef NDEBUG
    first_was_teleported = true;
    #endif
    assert( teleported_collision.portalIndex0() < m_state.planarPortals().size() );
    if( m_state.planarPortals()[teleported_collision.portalIndex0()].isLeesEdwards() )
    {
      portal0_is_lees_edwards = true;
    }
  }

  // Determine whether the second body was teleported
  bool portal1_is_lees_edwards{ false };
  #ifndef NDEBUG
  bool second_was_teleported{ false };
  #endif
  if( teleported_collision.portalIndex1() != std::numeric_limits<unsigned>::max() )
  {
    #ifndef NDEBUG
    second_was_teleported = true;
    #endif
    assert( teleported_collision.portalIndex1() < m_state.planarPortals().size() );
    if( m_state.planarPortals()[teleported_collision.portalIndex1()].isLeesEdwards() )
    {
      portal1_is_lees_edwards = true;
    }
  }

  // If neither portal is Lees-Edwards
  if( !portal0_is_lees_edwards && !portal1_is_lees_edwards )
  {
    switch( geo0->type() )
    {
      case RigidBody2DGeometry::CIRCLE:
      {
        const CircleGeometry& circle_geo0{ sd_cast<CircleGeometry&>( *geo0 ) };
        switch( geo1->type() )
        {
          case RigidBody2DGeometry::CIRCLE:
          {
            const CircleGeometry& circle_geo1{ sd_cast<CircleGeometry&>( *geo1 ) };
            if( CircleCircleConstraint::isActive( x0_t1, x1_t1, circle_geo0.r(), circle_geo1.r() ) )
            {
              // Creation of constraints at q0 to preserve angular momentum
              active_set.emplace_back( new TeleportedCircleCircleConstraint{ teleported_collision.bodyIndex0(), teleported_collision.bodyIndex1(), x0_t0, x1_t0, circle_geo0.r(), circle_geo1.r(), delta0_t0, delta1_t0, circle_geo0.r(), circle_geo1.r() } );
            }
            break;
          }
        }
        break;
      }
    }
  }
  // Otherwise, there is a relative velocity contribution from the Lees-Edwards boundary condition
  else
  {
    Vector2s kinematic_kick;
    assert( portal0_is_lees_edwards != portal1_is_lees_edwards );
    if( portal1_is_lees_edwards )
    {
      assert( second_was_teleported );
      // N.B. q1 because collision detection was performed with q1
      Array2s min;
      Array2s max;
      geo1->computeAABB( q1.segment<2>( 3 * teleported_collision.bodyIndex1() ), q1( 3 * teleported_collision.bodyIndex1() + 2 ), min, max );
      kinematic_kick = m_state.planarPortals()[teleported_collision.portalIndex1()].getKinematicVelocityOfAABB( min, max );
    }
    else // portal0_is_lees_edwards
    {
      assert( first_was_teleported );
      // N.B. q1 because collision detection was performed with q1
      Array2s min;
      Array2s max;
      geo0->computeAABB( q1.segment<2>( 3 * teleported_collision.bodyIndex0() ), q1( 3 * teleported_collision.bodyIndex0() + 2 ), min, max );
      kinematic_kick = -m_state.planarPortals()[teleported_collision.portalIndex0()].getKinematicVelocityOfAABB( min, max );
    }
    switch( geo0->type() )
    {
      case RigidBody2DGeometry::CIRCLE:
      {
        const CircleGeometry& circle_geo0{ sd_cast<CircleGeometry&>( *geo0 ) };
        switch( geo1->type() )
        {
          case RigidBody2DGeometry::CIRCLE:
          {
            const CircleGeometry& circle_geo1{ sd_cast<CircleGeometry&>( *geo1 ) };
            if( CircleCircleConstraint::isActive( x0_t1, x1_t1, circle_geo0.r(), circle_geo1.r() ) )
            {
              // Creation of constraints at q0 to preserve angular momentum
              active_set.emplace_back( new KinematicKickCircleCircleConstraint{ teleported_collision.bodyIndex0(), teleported_collision.bodyIndex1(), x0_t0, x1_t0, circle_geo0.r(), circle_geo1.r(), kinematic_kick } );
            }
            break;
          }
        }
        break;
      }
    }
  }
}

bool RigidBody2DSim::bodyCollidesWithAnother( const Vector2s& x, const scalar& theta, const std::unique_ptr<RigidBody2DGeometry>& geo, const VectorXs& q ) const
{
  assert( q.size() % 3 == 0 );

  const unsigned nbodies{ static_cast<unsigned>( q.size() / 3 ) };

  // Compute an AABB for the trial body
  Array2s trial_min;
  Array2s trial_max;
  geo->computeAABB( x, theta, trial_min, trial_max );
  const AABB trial_aabb{ trial_min, trial_max };

  // Candidate bodies that might overlap
  std::vector<unsigned> possible_overlaps;
  // Map from teleported AABB indices and body and portal indices
  std::map<unsigned,TeleportedBody> teleported_aabb_body_indices;
  {
    // Compute an AABB for each body
    std::vector<AABB> aabbs;
    aabbs.reserve( nbodies );
    for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
    {
      Array2s min;
      Array2s max;
      m_state.bodyGeometry( bdy_idx )->computeAABB( q.segment<2>( 3 * bdy_idx ), q( 3 * bdy_idx + 2 ), min, max );
      aabbs.emplace_back( min, max );
    }
    assert( aabbs.size() == nbodies );

    // Compute an AABB for each teleported body
    std::map<unsigned,TeleportedBody>::iterator aabb_bdy_map_itr{ teleported_aabb_body_indices.begin() };
    // For each portal
    for( const PlanarPortal& planar_portal : m_state.planarPortals() )
    {
      // For each body
      for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
      {
        // If the body is inside a portal
        bool intersecting_plane_index;
        if( planar_portal.aabbTouchesPortal( aabbs[bdy_idx].min(), aabbs[bdy_idx].max(), intersecting_plane_index )  )
        {
          // Teleport to the other side of the portal
          Vector2s x_out;
          planar_portal.teleportPoint( q.segment<2>( 3 * bdy_idx ), intersecting_plane_index, x_out );
          // Compute an AABB for the teleported particle
          Array2s min;
          Array2s max;
          m_state.bodyGeometry( bdy_idx )->computeAABB( x_out, q( 3 * bdy_idx + 2 ), min, max );
          aabbs.emplace_back( min, max );

          const unsigned prtl_idx{ Utilities::index( m_state.planarPortals(), planar_portal ) };
          aabb_bdy_map_itr = teleported_aabb_body_indices.insert( aabb_bdy_map_itr, std::make_pair( aabbs.size() - 1, TeleportedBody{ bdy_idx, prtl_idx, intersecting_plane_index } ) );
        }
      }
    }

    // Determine which bodies possibly overlap
    SpatialGrid::getPotentialOverlaps( trial_aabb, aabbs, possible_overlaps );
  }

  // Narrow phase checks to verify whether the bodies actually overlap
  for( const unsigned other_idx : possible_overlaps )
  {
    // If the other body wasn't teleported
    if( other_idx < nbodies )
    {
      // We can run standard narrow phase
      if( collisionIsActive( x, theta, geo, q.segment<2>( 3 * other_idx ), q( 3 * other_idx + 2 ), m_state.bodyGeometry( other_idx ) ) )
      {
        return true;
      }
    }
    // If the other body was teleported
    else
    {
      // TODO: Never actually seem to hit this else clause randomly. Set up an artificial example that purpousefully executes this code.
      std::cout << "!!!! OTHER TELEPORTED !!!!!" << std::endl;
      const std::map<unsigned,TeleportedBody>::const_iterator map_itr{ teleported_aabb_body_indices.find( other_idx ) };
      assert( map_itr != teleported_aabb_body_indices.end() );
      const unsigned other_bdy_idx{ map_itr->second.bodyIndex() };
      assert( other_bdy_idx < nbodies );
      const unsigned prtl_idx{ map_itr->second.portalIndex() };
      assert( prtl_idx < m_state.planarPortals().size() );
      const unsigned prtl_plane{ map_itr->second.planeIndex() };

      // Check if the teleported collision happens
      Vector2s teleported_center{ q.segment<2>( 3 * other_bdy_idx ) };
      getTeleportedCollisionCenter( prtl_idx, prtl_plane, teleported_center );
      if( collisionIsActive( x, theta, geo, teleported_center, q( 3 * other_bdy_idx + 2 ), m_state.bodyGeometry( other_bdy_idx ) ) )
      {
        return true;
      }
    }
  }
  return false;
}

void RigidBody2DSim::computeBodyBodyActiveSetAllPairs( const VectorXs& q0, const VectorXs& q1, std::vector<std::unique_ptr<Constraint>>& active_set ) const
{
  assert( q0.size() % 3 == 0 ); assert( q0.size() == q1.size() );

  const unsigned nbodies{ static_cast<unsigned>( q0.size() / 3 ) };

  // Check all body-body pairs
  for( unsigned bdy_idx_0 = 0; bdy_idx_0 < nbodies; ++bdy_idx_0 )
  {
    for( unsigned bdy_idx_1 = bdy_idx_0 + 1; bdy_idx_1 < nbodies; ++bdy_idx_1 )
    {
      const std::unique_ptr<RigidBody2DGeometry>& geo0{ m_state.geometry()[ m_state.geometryIndices()( bdy_idx_0 ) ] };
      const std::unique_ptr<RigidBody2DGeometry>& geo1{ m_state.geometry()[ m_state.geometryIndices()( bdy_idx_1 ) ] };
      dispatchNarrowPhaseCollision( bdy_idx_0, bdy_idx_1, geo0, geo1, q0, q1, active_set );
    }
  }
}

void RigidBody2DSim::computeBodyPlaneActiveSetAllPairs( const VectorXs& q0, const VectorXs& q1, std::vector<std::unique_ptr<Constraint>>& active_set ) const
{
  assert( q0.size() % 3 == 0 ); assert( q0.size() == q1.size() );

  const unsigned nbodies{ static_cast<unsigned>( q0.size() / 3 ) };

  // Check all body-plane pairs
  for( unsigned plane_idx = 0; plane_idx < m_state.planes().size(); ++plane_idx )
  {
    for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
    {
      switch( m_state.geometry()[ m_state.geometryIndices()( bdy_idx ) ]->type() )
      {
        case RigidBody2DGeometry::CIRCLE:
        {
          const CircleGeometry& circle_geo{ sd_cast<CircleGeometry&>( *m_state.geometry()[ m_state.geometryIndices()( bdy_idx ) ] ) };
          if( StaticPlaneCircleConstraint::isActive( q1.segment<2>( 3 * bdy_idx ), circle_geo.r(), m_state.planes()[plane_idx] ) )
          {
            active_set.emplace_back( new StaticPlaneCircleConstraint{ bdy_idx, plane_idx, circle_geo.r(), m_state.planes()[plane_idx] } );
          }
          break;
        }
      }
    }
  }
}

void RigidBody2DSim::computeActiveSet( const VectorXs& q0, const VectorXs& q1, std::vector<std::unique_ptr<Constraint>>& active_set )
{
  assert( q0.size() % 3 == 0 ); assert( q0.size() == q1.size() );

  active_set.clear();

  // Detect body-body collisions
  //computeBodyBodyActiveSetAllPairs( q0, q1, active_set );
  computeBodyBodyActiveSetSpatialGrid( q0, q1, active_set );

  // Check all ball-drum pairs
  //computeBallDrumActiveSetAllPairs( q0, qp, active_set );

  // Check all body-plane pairs
  computeBodyPlaneActiveSetAllPairs( q0, q1, active_set );
}

void RigidBody2DSim::computeImpactBases( const VectorXs& q, const std::vector<std::unique_ptr<Constraint>>& active_set, MatrixXXsc& impact_bases ) const
{
  const unsigned ncols{ static_cast<unsigned>( active_set.size() ) };
  impact_bases.resize( 2, ncols );
  for( unsigned col_num = 0; col_num < ncols; ++col_num )
  {
    VectorXs current_normal;
    active_set[col_num]->getWorldSpaceContactNormal( q, current_normal );
    assert( fabs( current_normal.norm() - 1.0 ) <= 1.0e-6 );
    impact_bases.col( col_num ) = current_normal;
  }
}

void RigidBody2DSim::computeContactBases( const VectorXs& q, const VectorXs& v, const std::vector<std::unique_ptr<Constraint>>& active_set, MatrixXXsc& contact_bases ) const
{
  const unsigned ncols{ static_cast<unsigned>( active_set.size() ) };
  contact_bases.resize( 2, 2 * ncols );
  for( unsigned col_num = 0; col_num < ncols; ++col_num )
  {
    MatrixXXsc basis;
    active_set[col_num]->computeBasis( q, v, basis );
    assert( basis.rows() == basis.cols() ); assert( basis.rows() == 2 );
    assert( ( basis * basis.transpose() - MatrixXXsc::Identity( 2, 2 ) ).lpNorm<Eigen::Infinity>() <= 1.0e-6 );
    assert( fabs( basis.determinant() - 1.0 ) <= 1.0e-6 );
    contact_bases.block<2,2>( 0, 2 * col_num ) = basis;
  }
}

void RigidBody2DSim::clearConstraintCache()
{
  //m_constraint_cache.clear();
}

void RigidBody2DSim::cacheConstraint( const Constraint& constraint, const VectorXs& r )
{
  std::cerr << "RigidBody2DSim::cacheConstraint" << std::endl;
  std::exit( EXIT_FAILURE );
//  m_constraint_cache.cacheConstraint( constraint, r );
}

void RigidBody2DSim::getCachedConstraintImpulse( const Constraint& constraint, VectorXs& r ) const
{
  std::cerr << "TwoDBallSim::cacheConstraint" << std::endl;
  std::exit( EXIT_FAILURE );
//  m_constraint_cache.getCachedConstraint( constraint, r );
}

bool RigidBody2DSim::equal( const Constraint& constraint0, const Constraint& constraint1 ) const
{
  const std::type_info& type0{ typeid(constraint0) };
  const std::type_info& type1{ typeid(constraint1) };

  if( type0 != type1 )
  {
    return false;
  }

  if( type0 == typeid(StaticPlaneCircleConstraint) )
  {
    const StaticPlaneCircleConstraint& static_plane_constraint_0{ sd_cast<const StaticPlaneCircleConstraint&>( constraint0 ) };
    const StaticPlaneCircleConstraint& static_plane_constraint_1{ sd_cast<const StaticPlaneCircleConstraint&>( constraint1 ) };
    return static_plane_constraint_0 == static_plane_constraint_1;
  }
  else if( type0 == typeid(CircleCircleConstraint) )
  {
    const CircleCircleConstraint& circle_circle_constraint_0{ sd_cast<const CircleCircleConstraint&>( constraint0 ) };
    const CircleCircleConstraint& circle_circle_constraint_1{ sd_cast<const CircleCircleConstraint&>( constraint1 ) };
    return circle_circle_constraint_0 == circle_circle_constraint_1;
  }
  else if( type0 == typeid(TeleportedCircleCircleConstraint) )
  {
    const TeleportedCircleCircleConstraint& teleported_circle_circle_constraint_0{ sd_cast<const TeleportedCircleCircleConstraint&>( constraint0 ) };
    const TeleportedCircleCircleConstraint& teleported_circle_circle_constraint_1{ sd_cast<const TeleportedCircleCircleConstraint&>( constraint1 ) };
    return teleported_circle_circle_constraint_0 == teleported_circle_circle_constraint_1;
  }
  else
  {
    std::cout << "Unsupported constraint type in RigidBody2DSim::equal: " << constraint0.name() << std::endl;
    std::exit( EXIT_FAILURE );
  }
}

void RigidBody2DSim::flow( const unsigned iteration, const scalar& dt, UnconstrainedMap& umap )
{
  VectorXs q1{ m_state.q().size() };
  VectorXs v1{ m_state.v().size() };

  updatePeriodicBoundaryConditionsStartOfStep( iteration, dt );

  umap.flow( m_state.q(), m_state.v(), *this, iteration, dt, q1, v1 );

  q1.swap( m_state.q() );
  v1.swap( m_state.v() );

  enforcePeriodicBoundaryConditions( m_state.q() );
}

void RigidBody2DSim::flow( const unsigned iteration, const scalar& dt, UnconstrainedMap& umap, ImpactOperator& iop, const scalar& CoR, ImpactMap& imap )
{
  VectorXs q1{ m_state.q().size() };
  VectorXs v1{ m_state.v().size() };

  updatePeriodicBoundaryConditionsStartOfStep( iteration, dt );

  PythonScripting scripting;
  imap.flow( scripting, *this, *this, umap, iop, iteration, dt, CoR, m_state.q(), m_state.v(), q1, v1 );

  q1.swap( m_state.q() );
  v1.swap( m_state.v() );

  enforcePeriodicBoundaryConditions( m_state.q() );
}

void RigidBody2DSim::flow( const unsigned iteration, const scalar& dt, UnconstrainedMap& umap, const scalar& CoR, const scalar& mu, FrictionSolver& solver, ImpactFrictionMap& ifmap )
{
  VectorXs q1{ m_state.q().size() };
  VectorXs v1{ m_state.v().size() };

  updatePeriodicBoundaryConditionsStartOfStep( iteration, dt );

  PythonScripting scripting;
  ifmap.flow( scripting, *this, *this, umap, solver, iteration, dt, CoR, mu, m_state.q(), m_state.v(), q1, v1 );

  q1.swap( m_state.q() );
  v1.swap( m_state.v() );

  enforcePeriodicBoundaryConditions( m_state.q() );
}

void RigidBody2DSim::updatePeriodicBoundaryConditionsStartOfStep( const unsigned next_iteration, const scalar& dt )
{
  const scalar t{ next_iteration * dt };
  for( PlanarPortal& planar_portal : m_state.planarPortals() )
  {
    planar_portal.updateMovingPortals( t );
  }
}

void RigidBody2DSim::enforcePeriodicBoundaryConditions( VectorXs& q )
{
  assert( q.size() % 3 == 0 );

  const unsigned nbodies{ static_cast<unsigned>( q.size() / 3 ) };

  // For each portal
  for( const PlanarPortal& planar_portal : m_state.planarPortals() )
  {
    // For each body
    for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
    {
      // TODO: Calling pointInsidePortal and teleportPointInsidePortal is a bit redundant, clean this up!
      // If the body is inside a portal
      if( planar_portal.pointInsidePortal( q.segment<2>( 3 * bdy_idx ) ) )
      {
        // Teleport to the other side of the portal
        Vector2s x_out;
        planar_portal.teleportPointInsidePortal( q.segment<2>( 3 * bdy_idx ), x_out );
        q.segment<2>( 3 * bdy_idx ) = x_out;
      }
    }
  }
}

void RigidBody2DSim::computeBodyBodyActiveSetSpatialGrid( const VectorXs& q0, const VectorXs& q1, std::vector<std::unique_ptr<Constraint>>& active_set ) const
{
  assert( q0.size() % 3 == 0 ); assert( q0.size() == q1.size() );

  const unsigned nbodies{ static_cast<unsigned>( q0.size() / 3 ) };

  // Candidate bodies that might overlap
  std::set<std::pair<unsigned,unsigned>> possible_overlaps;
  // Map from teleported AABB indices and body and portal indices
  std::map<unsigned,TeleportedBody> teleported_aabb_body_indices;
  {
    // Compute an AABB for each body
    std::vector<AABB> aabbs;
    aabbs.reserve( nbodies );
    for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
    {
      Array2s min;
      Array2s max;
      m_state.bodyGeometry( bdy_idx )->computeAABB( q1.segment<2>( 3 * bdy_idx ), q1( 3 * bdy_idx + 2 ), min, max );
      aabbs.emplace_back( min, max );
    }
    assert( aabbs.size() == nbodies );

    // Compute an AABB for each teleported body
    std::map<unsigned,TeleportedBody>::iterator aabb_bdy_map_itr{ teleported_aabb_body_indices.begin() };
    // For each portal
    for( const PlanarPortal& planar_portal : m_state.planarPortals() )
    {
      // For each body
      for( unsigned bdy_idx = 0; bdy_idx < nbodies; ++bdy_idx )
      {
        // If the body is inside a portal
        bool intersecting_plane_index;
        if( planar_portal.aabbTouchesPortal( aabbs[bdy_idx].min(), aabbs[bdy_idx].max(), intersecting_plane_index )  )
        {
          // Teleport to the other side of the portal
          Vector2s x_out;
          planar_portal.teleportPoint( q1.segment<2>( 3 * bdy_idx ), intersecting_plane_index, x_out );
          // Compute an AABB for the teleported particle
          Array2s min;
          Array2s max;
          m_state.bodyGeometry( bdy_idx )->computeAABB( x_out, q1( 3 * bdy_idx + 2 ), min, max );
          aabbs.emplace_back( min, max );

          const unsigned prtl_idx{ Utilities::index( m_state.planarPortals(), planar_portal ) };
          aabb_bdy_map_itr = teleported_aabb_body_indices.insert( aabb_bdy_map_itr, std::make_pair( aabbs.size() - 1, TeleportedBody{ bdy_idx, prtl_idx, intersecting_plane_index } ) );
        }
      }
    }

    // Determine which bodies possibly overlap
    SpatialGrid::getPotentialOverlaps( aabbs, possible_overlaps );
  }

  std::set<TeleportedCollision> teleported_collisions;

  #ifndef NDEBUG
  std::vector<std::pair<unsigned,unsigned>> duplicate_indices;
  #endif

  // Create constraints for bodies that actually overlap
  for( const std::pair<unsigned,unsigned>& possible_overlap_pair : possible_overlaps )
  {
    const bool first_teleported{ possible_overlap_pair.first >= nbodies };
    const bool second_teleported{ possible_overlap_pair.second >= nbodies };

    // If neither body in the current collision was teleported
    if( !first_teleported && !second_teleported )
    {
      // We can run standard narrow phase
      dispatchNarrowPhaseCollision( possible_overlap_pair.first, possible_overlap_pair.second, m_state.bodyGeometry( possible_overlap_pair.first ), m_state.bodyGeometry( possible_overlap_pair.second ), q0, q1, active_set );
    }
    // If at least one of the balls was teleported
    else
    {
      unsigned bdy_idx_0{ possible_overlap_pair.first };
      unsigned bdy_idx_1{ possible_overlap_pair.second };
      unsigned prtl_idx_0{ std::numeric_limits<unsigned>::max() };
      unsigned prtl_idx_1{ std::numeric_limits<unsigned>::max() };
      bool prtl_plane_0{ 0 };
      bool prtl_plane_1{ 0 };

      if( first_teleported )
      {
        const std::map<unsigned,TeleportedBody>::const_iterator map_itr{ teleported_aabb_body_indices.find( possible_overlap_pair.first ) };
        assert( map_itr != teleported_aabb_body_indices.end() );
        bdy_idx_0 = map_itr->second.bodyIndex();
        assert( bdy_idx_0 < nbodies );
        prtl_idx_0 = map_itr->second.portalIndex();
        assert( prtl_idx_0 < m_state.planarPortals().size() );
        prtl_plane_0 = map_itr->second.planeIndex();
      }
      if( second_teleported )
      {
        const std::map<unsigned,TeleportedBody>::const_iterator map_itr{ teleported_aabb_body_indices.find( possible_overlap_pair.second ) };
        assert( map_itr != teleported_aabb_body_indices.end() );
        bdy_idx_1 = map_itr->second.bodyIndex();
        assert( bdy_idx_1 < nbodies );
        prtl_idx_1 = map_itr->second.portalIndex();
        assert( prtl_idx_1 < m_state.planarPortals().size() );
        prtl_plane_1 = map_itr->second.planeIndex();
      }

      // Check if the collision will be detected in the unteleported state
      if( first_teleported && second_teleported )
      {
        if( collisionIsActive( bdy_idx_0, bdy_idx_1, m_state.bodyGeometry( bdy_idx_0 ), m_state.bodyGeometry( bdy_idx_1 ), q1 ) )
        {
          #ifndef NDEBUG
          duplicate_indices.push_back( std::make_pair( bdy_idx_0, bdy_idx_1 ) );
          #endif
          continue;
        }
      }

      // Check if the teleported collision happens
      const TeleportedCollision possible_collision{ bdy_idx_0, bdy_idx_1, prtl_idx_0,  prtl_idx_1, prtl_plane_0, prtl_plane_1 };
      if( teleportedCollisionIsActive( possible_collision, m_state.bodyGeometry( bdy_idx_0 ), m_state.bodyGeometry( bdy_idx_1 ), q1 ) )
      {
        teleported_collisions.insert( possible_collision );
      }
    }
  }
  possible_overlaps.clear();
  teleported_aabb_body_indices.clear();

  #ifndef NDEBUG
  // Double check that non-teleport duplicate collisions were actually duplicates
  for( const std::pair<unsigned,unsigned>& dup_col : duplicate_indices )
  {
    bool entry_found{ false };
    const unsigned dup_idx_0{ std::min( dup_col.first, dup_col.second ) };
    const unsigned dup_idx_1{ std::max( dup_col.first, dup_col.second ) };
    for( const std::unique_ptr<Constraint>& col : active_set )
    {
      std::pair<int,int> bodies;
      col->getBodyIndices( bodies );
      if( bodies.first < 0 || bodies.second < 0 )
      {
        continue;
      }
      const unsigned idx_0{ std::min( unsigned( bodies.first ), unsigned( bodies.second ) ) };
      const unsigned idx_1{ std::max( unsigned( bodies.first ), unsigned( bodies.second ) ) };
      if( dup_idx_0 == idx_0 && dup_idx_1 == idx_1 )
      {
        entry_found = true;
        break;
      }
    }
    assert( entry_found );
  }
  #endif

  // Create constraints for teleported collisions
  for( const TeleportedCollision& teleported_collision : teleported_collisions )
  {
    assert( teleported_collision.bodyIndex0() < nbodies ); assert( teleported_collision.bodyIndex1() < nbodies );
    assert( teleported_collision.bodyIndex0() != teleported_collision.bodyIndex1( ) );
    dispatchTeleportedNarrowPhaseCollision( teleported_collision, m_state.bodyGeometry( teleported_collision.bodyIndex0() ), m_state.bodyGeometry( teleported_collision.bodyIndex1() ), q0, q1, active_set );
  }

  #ifndef NDEBUG
  // Do an all pairs check for duplicates
  const unsigned ncons{ static_cast<unsigned>( active_set.size() ) };
  for( unsigned con_idx_0 = 0; con_idx_0 < ncons; ++con_idx_0 )
  {
    std::pair<int,int> indices0;
    active_set[con_idx_0]->getBodyIndices( indices0 );
    const int idx_0a{ std::min( indices0.first, indices0.second ) };
    const int idx_1a{ std::max( indices0.first, indices0.second ) };
    for( unsigned con_idx_1 = con_idx_0 + 1; con_idx_1 < ncons; ++con_idx_1 )
    {
      std::pair<int,int> indices1;
      active_set[con_idx_1]->getBodyIndices( indices1 );
      const int idx_0b{ std::min( indices1.first, indices1.second ) };
      const int idx_1b{ std::max( indices1.first, indices1.second ) };
      assert( !( ( idx_0a == idx_0b ) && ( idx_1a == idx_1b ) ) );
    }
  }
  #endif
}

// TODO: 0 size plane matrices are not output due to a bug in an older version of HDF5
void RigidBody2DSim::writeBinaryState( HDF5File& output_file ) const
{
  // Output the configuration
  output_file.writeMatrix( "", "q", m_state.q() );
  // Output the velocity
  output_file.writeMatrix( "", "v", m_state.v() );
  // Output the mass
  {
    // Assemble the mass into a single flat vector like q, v, and r
    assert( m_state.M().nonZeros() == m_state.q().size() );
    const VectorXs m{ Eigen::Map<const VectorXs>{ &m_state.M().data().value(0), m_state.q().size() } };
    output_file.writeMatrix( "", "m", m );
  }
  // Output the simulated geometry
  output_file.createGroup( "geometry" );
  RigidBody2DStateOutput::writeGeometryIndices( m_state.geometry(), m_state.geometryIndices(), "geometry", output_file );
  RigidBody2DStateOutput::writeGeometry( m_state.geometry(), "geometry", output_file );
  // Output the static geometry
  output_file.createGroup( "static_geometry" );
  if( !m_state.planes().empty() )
  {
    RigidBody2DStateOutput::writeStaticPlanes( m_state.planes(), "static_geometry", output_file );
  }
  if( !m_state.planarPortals().empty() )
  {
    RigidBody2DStateOutput::writePlanarPortals( m_state.planarPortals(), "static_geometry", output_file );
  }
}

void RigidBody2DSim::serialize( std::ostream& output_stream ) const
{
  assert( output_stream.good() );
  m_state.serialize( output_stream );
  //m_constraint_cache.serialize( output_stream );
}

void RigidBody2DSim::deserialize( std::istream& input_stream )
{
  assert( input_stream.good() );
  m_state.deserialize( input_stream );
  //m_constraint_cache.deserialize( input_stream );
}