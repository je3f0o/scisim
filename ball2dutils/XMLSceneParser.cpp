// XMLSceneParser.cpp
//
// Breannan Smith
// Last updated: 09/05/2015

#include "XMLSceneParser.h"

#include <iostream>
#include <fstream>

#include "Ball2D.h"

#include "ball2d/Forces/Ball2DForce.h"
#include "ball2d/Forces/Ball2DGravityForce.h"
#include "ball2d/Forces/HertzianPenaltyForce.h"
#include "ball2d/StaticGeometry/StaticDrum.h"
#include "ball2d/StaticGeometry/StaticPlane.h"
#include "ball2d/Portals/PlanarPortal.h"
#include "ball2d/VerletMap.h"
#include "ball2d/SymplecticEulerMap.h"

#include "scisim/StringUtilities.h"
#include "scisim/Math/Rational.h"
#include "scisim/UnconstrainedMaps/UnconstrainedMap.h"
#include "scisim/ConstrainedMaps/ImpactMaps/ImpactMap.h"
#include "scisim/ConstrainedMaps/GeometricImpactFrictionMap.h"
#include "scisim/ConstrainedMaps/StabilizedImpactFrictionMap.h"
#include "scisim/ConstrainedMaps/FrictionMaps/BoundConstrainedMDPOperatorQL.h"
#include "scisim/ConstrainedMaps/FrictionMaps/BoundConstrainedMDPOperatorIpopt.h"
#include "scisim/ConstrainedMaps/ImpactMaps/ImpactOperator.h"
#include "scisim/ConstrainedMaps/ImpactMaps/GaussSeidelOperator.h"
#include "scisim/ConstrainedMaps/ImpactMaps/JacobiOperator.h"
#include "scisim/ConstrainedMaps/ImpactMaps/LCPOperatorQL.h"
#include "scisim/ConstrainedMaps/ImpactMaps/LCPOperatorQLVP.h"
#include "scisim/ConstrainedMaps/ImpactMaps/LCPOperatorIpopt.h"
#include "scisim/ConstrainedMaps/ImpactMaps/GROperator.h"
#include "scisim/ConstrainedMaps/ImpactMaps/GRROperator.h"
#include "scisim/ConstrainedMaps/FrictionSolver.h"
#include "scisim/ConstrainedMaps/StaggeredProjections.h"
#include "scisim/ConstrainedMaps/Sobogus.h"

#include "rapidxml.hpp"

static bool loadTextFileIntoVector( const std::string& filename, std::vector<char>& xmlchars )
{
  assert( xmlchars.empty() );

  // Attempt to open the text file for reading
  std::ifstream textfile{ filename };
  if( !textfile.is_open() )
  {
    return false;
  }

  // Read the entire file into a single string
  std::string line;
  while( getline( textfile, line ) )
  {
    std::copy( line.begin(), line.end(), back_inserter( xmlchars ) );
  }
  xmlchars.emplace_back( '\0' );

  return true;
}

static bool loadXMLFile( const std::string& filename, std::vector<char>& xmlchars, rapidxml::xml_document<>& doc )
{
  assert( xmlchars.empty() );

  // Attempt to read the text from the user-specified xml file
  if( !loadTextFileIntoVector( filename, xmlchars ) )
  {
    std::cerr << "Failed to read scene file: " << filename << std::endl;
    return false;
  }

  // Initialize the xml parser with the character vector
  try
  {
    doc.parse<0>( &xmlchars.front() );
  }
  catch( const rapidxml::parse_error& e )
  {
    std::cerr << "Failed to parse scene file: " << filename << std::endl;
    std::cerr << "Error message: " << e.what() << std::endl;
    return false;
  }

  return true;
}

static bool loadCameraSettings( const rapidxml::xml_node<>& node, Eigen::Vector2d& camera_center, double& camera_scale_factor, unsigned& fps, bool& render_at_fps, bool& lock_camera )
{
  // Attempt to parse the camera's center
  {
    const rapidxml::xml_attribute<>* cx_attrib{ node.first_attribute( "cx" ) };
    if( !cx_attrib )
    {
      std::cerr << "Failed to locate cx attribute for camera" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( cx_attrib->value(), camera_center.x() ) )
    {
      std::cerr << "Failed to parse cx attribute for camera. Value must be a scalar." << std::endl;
      return false;
    }
  }
  {
    const rapidxml::xml_attribute<>* cy_attrib{ node.first_attribute( "cy" ) };
    if( !cy_attrib )
    {
      std::cerr << "Failed to locate cy attribute for camera" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( cy_attrib->value(), camera_center.y() ) )
    {
      std::cerr << "Failed to parse cy attribute for camera. Value must be a scalar." << std::endl;
      return false;
    }
  }

  // Attempt to parse the scale setting
  {
    const rapidxml::xml_attribute<>* scale_factor_attrib{ node.first_attribute( "scale_factor" ) };
    if( !scale_factor_attrib )
    {
      std::cerr << "Failed to locate scale_factor attribute for camera" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( scale_factor_attrib->value(), camera_scale_factor ) )
    {
      std::cerr << "Failed to parse scale_factor attribute for camera. Value must be a scalar." << std::endl;
      return false;
    }
  }

  // Attempt to parse the fps setting
  {
    const rapidxml::xml_attribute<>* fps_attrib{ node.first_attribute( "fps" ) };
    if( fps_attrib == nullptr )
    {
      std::cerr << "Failed to locate fps attribute for camera" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( fps_attrib->value(), fps ) )
    {
      std::cerr << "Failed to parse fps attribute for camera. Value must be a non-negative integer." << std::endl;
      return false;
    }
  }

  // Attempt to parse the render_at_fps setting
  {
    const rapidxml::xml_attribute<>* render_at_fps_attrib{ node.first_attribute( "render_at_fps" ) };
    if( render_at_fps_attrib == nullptr )
    {
      std::cerr << "Failed to locate render_at_fps attribute for camera" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( render_at_fps_attrib->value(), render_at_fps ) )
    {
      std::cerr << "Failed to parse render_at_fps attribute for camera. Value must be a boolean." << std::endl;
      return false;
    }
  }

  // Attempt to parse the locked setting
  {
    const rapidxml::xml_attribute<>* locked_attrib{ node.first_attribute( "locked" ) };
    if( locked_attrib == nullptr )
    {
      std::cerr << "Failed to locate locked attribute for camera" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( locked_attrib->value(), lock_camera ) )
    {
      std::cerr << "Failed to parse locked attribute for camera. Value must be a boolean." << std::endl;
      return false;
    }
  }

  return true;
}

static bool loadEndTime( const rapidxml::xml_node<>& node, scalar& end_time )
{
  // Attempt to parse the time setting
  const rapidxml::xml_attribute<>* t_attrib{ node.first_attribute( "t" ) };
  if( !t_attrib )
  {
    std::cerr << "Failed to locate t attribute for end_time node." << std::endl;
    return false;
  }
  if( !StringUtilities::extractFromString( t_attrib->value(), end_time ) )
  {
    std::cerr << "Failed to parse t attribute for end_time. Value must be a positive scalar." << std::endl;
    return false;
  }
  if( end_time <= 0.0 )
  {
    std::cerr << "Failed to parse t attribute for end_time. Value must be a positive scalar." << std::endl;
    return false;

  }

  return true;
}

static bool loadScriptingSetup( const rapidxml::xml_node<>& node, std::string& scripting_callback )
{
  assert( scripting_callback.empty() );

  const rapidxml::xml_node<>* scripting_node{ node.first_node( "scripting" ) };
  if( !scripting_node )
  {
    return true;
  }

  const rapidxml::xml_attribute<>* name_node{ scripting_node->first_attribute( "callback" ) };
  if( name_node )
  {
    scripting_callback = name_node->value();
  }
  else
  {
    return false;
  }

  return true;
}

static bool loadBalls( const rapidxml::xml_node<>& node, std::vector<Ball2D>& balls )
{
  for( rapidxml::xml_node<>* nd = node.first_node( "ball" ); nd; nd = nd->next_sibling( "ball" ) )
  {
    // Attempt to parse the ball's position
    Vector2s x;

    const rapidxml::xml_attribute<>* const x_attrib{ nd->first_attribute( "x" ) };
    if( !x_attrib ) { return false; }
    StringUtilities::extractFromString( x_attrib->value(), x.x() );

    const rapidxml::xml_attribute<>* const y_attrib{ nd->first_attribute( "y" ) };
    if( !y_attrib ) { return false; }
    StringUtilities::extractFromString( y_attrib->value(), x.y() );

    // Attempt to parse the ball's velocity
    Vector2s v;

    const rapidxml::xml_attribute<>* const vx_attrib{ nd->first_attribute( "vx" ) };
    if( !vx_attrib ) { return false; }
    StringUtilities::extractFromString( vx_attrib->value(), v.x() );

    const rapidxml::xml_attribute<>* const vy_attrib{ nd->first_attribute( "vy" ) };
    if( !vy_attrib ) { return false; }
    StringUtilities::extractFromString( vy_attrib->value(), v.y() );

    // Attempt to parse the ball's mass
    scalar m;
    const rapidxml::xml_attribute<>* const m_attrib{ nd->first_attribute( "m" ) };
    if( !m_attrib ) { return false; }
    StringUtilities::extractFromString( m_attrib->value(), m );

    // Attempt to parse the ball's radius
    scalar r;
    const rapidxml::xml_attribute<>* const r_attrib{ nd->first_attribute( "r" ) };
    if( !r_attrib ) { return false; }
    StringUtilities::extractFromString( r_attrib->value(), r );

    // Attempt to parse whether the ball is fixed
    bool fixed;
    const rapidxml::xml_attribute<>* const fixed_attrib{ nd->first_attribute( "fixed" ) };
    if( !fixed_attrib ) { return false; }
    StringUtilities::extractFromString( fixed_attrib->value(), fixed );

    balls.emplace_back( x, v, m, r, fixed );
  }

  return true;
}

static bool loadStaticDrums( const rapidxml::xml_node<>& node, std::vector<StaticDrum>& drums )
{
  for( rapidxml::xml_node<>* nd = node.first_node( "static_drum" ); nd; nd = nd->next_sibling( "static_drum" ) )
  {
    // Attempt to parse the drums's position
    Vector2s x;

    const rapidxml::xml_attribute<>* const x_attrib{ nd->first_attribute( "x" ) };
    if( !x_attrib ) { return false; }
    StringUtilities::extractFromString( x_attrib->value(), x.x() );

    const rapidxml::xml_attribute<>* const y_attrib{ nd->first_attribute( "y" ) };
    if( !y_attrib ) { return false; }
    StringUtilities::extractFromString( y_attrib->value(), x.y() );

    // Attempt to parse the drums's radius
    scalar r;
    const rapidxml::xml_attribute<>* const r_attrib{ nd->first_attribute( "r" ) };
    if( !r_attrib ) { return false; }
    StringUtilities::extractFromString( r_attrib->value(), r );

    drums.emplace_back( x, r );
  }

  return true;
}

static bool loadStaticPlanes( const rapidxml::xml_node<>& node, std::vector<StaticPlane>& planes )
{
  for( rapidxml::xml_node<>* nd = node.first_node( "static_plane" ); nd; nd = nd->next_sibling( "static_plane" ) )
  {
    // Read a point on the plane
    Vector2s x;
    {
      const rapidxml::xml_attribute<>* const attrib{ nd->first_attribute( "x" ) };
      if( !attrib ) { return false; }

      std::stringstream ss;
      ss << attrib->value();
      scalar component;
      std::vector<scalar> components;
      while( ss >> component ) { components.emplace_back( component ); }
      if( components.size() != 2 )
      {
        std::cerr << "Invalid number of components for static_plane x. Two required." << std::endl;
        return false;
      }

      x << components[0], components[1];
    }

    // Read the normal
    Vector2s n;
    {
      const rapidxml::xml_attribute<>* const attrib{ nd->first_attribute( "n" ) };
      if( !attrib ) { return false; }

      std::stringstream ss;
      ss << attrib->value();
      scalar component;
      std::vector<scalar> components;
      while( ss >> component ) { components.emplace_back( component ); }
      if( components.size() != 2 )
      {
        std::cerr << "Invalid number of components for static_plane n. Two required." << std::endl;
        return false;
      }

      n << components[0], components[1];
    }

    planes.emplace_back( x, n );
  }

  return true;
}

// TODO: minus ones here can underflow
static bool loadPlanarPortals( const rapidxml::xml_node<>& node, std::vector<StaticPlane>& planes, std::vector<PlanarPortal>& planar_portals )
{
  if( !( node.first_node( "planar_portal" ) || node.first_node( "lees_edwards_portal" ) )  )
  {
    return true;
  }

  // If we have a portal we must have at least one plane
  if( planes.size() < 2 )
  {
    std::cerr << "Must provide at least two planes before instantiating a planar portal." << std::endl;
    return false;
  }

  // Pairs of planes to turn into portals
  std::vector<std::pair<unsigned,unsigned>> plane_pairs;
  std::vector<std::pair<scalar,scalar>> plane_tangent_velocities;
  std::vector<std::pair<Vector2s,Vector2s>> plane_bounds;

  // Load planes without kinematic velocities
  for( rapidxml::xml_node<>* nd = node.first_node( "planar_portal" ); nd; nd = nd->next_sibling( "planar_portal" ) )
  {
    // Read the first plane index
    const rapidxml::xml_attribute<>* const attrib_a{ nd->first_attribute( "planeA" ) };
    if( !attrib_a )
    {
      std::cerr << "Failed to locate planeA attribute for planar_portal node." << std::endl;
      return false;
    }
    unsigned index_a;
    if( !StringUtilities::extractFromString( attrib_a->value(), index_a ) )
    {
      std::cerr << "Failed to parse planeA attribute for planar_portal node with value " << attrib_a->value() << ". Attribute must be an unsigned integer." << std::endl;
      return false;
    }
    if( index_a >= planes.size() )
    {
      std::cerr << "Failed to parse planeA attribute for planar_portal node with value " << attrib_a->value() << ". Attribute must be an index of a plane between " << 0 << " and " << planes.size() - 1 << std::endl;
      return false;
    }

    // Read the second plane index
    const rapidxml::xml_attribute<>* const attrib_b{ nd->first_attribute( "planeB" ) };
    if( !attrib_b )
    {
      std::cerr << "Failed to locate planeB attribute for planar_portal node." << std::endl;
      return false;
    }
    unsigned index_b;
    if( !StringUtilities::extractFromString( attrib_b->value(), index_b ) )
    {
      std::cerr << "Failed to parse planeB attribute for planar_portal node with value " << attrib_b->value() << ". Attribute must be an unsigned integer." << std::endl;
      return false;
    }
    if( index_b >= planes.size() )
    {
      std::cerr << "Failed to parse planeB attribute for planar_portal node with value " << attrib_b->value() << ". Attribute must be an index of a plane between " << 0 << " and " << planes.size() - 1 << std::endl;
      return false;
    }

    // Indices for this portal can't repeat
    if( index_a == index_b )
    {
      std::cerr << "Failed to parse planeB attribute for planar_portal node with value " << attrib_b->value() << ". Value is a repeat of attribute planeA." << std::endl;
      return false;
    }

    // Indices can not repeat previous portals
    for( std::vector<std::pair<unsigned,unsigned>>::size_type i = 0; i < plane_pairs.size(); ++i )
    {
      if( index_a == plane_pairs[i].first || index_a == plane_pairs[i].second )
      {
        std::cerr << "Failed to parse planeA attribute for planar_portal node with value " << attrib_a->value() << ". Plane index is used by an existing portal." << std::endl;
        return false;
      }
      if( index_b == plane_pairs[i].first || index_b == plane_pairs[i].second )
      {
        std::cerr << "Failed to parse planeB attribute for planar_portal node with value " << attrib_b->value() << ". Plane index is used by an existing portal." << std::endl;
        return false;
      }
    }

    plane_pairs.emplace_back( index_a, index_b );
    plane_tangent_velocities.emplace_back( 0.0, 0.0 );
    plane_bounds.emplace_back( Vector2s{ -SCALAR_INFINITY, SCALAR_INFINITY }, Vector2s{ -SCALAR_INFINITY, SCALAR_INFINITY } );
  }

  // Load planes with kinematic velocities
  for( rapidxml::xml_node<>* nd = node.first_node( "lees_edwards_portal" ); nd; nd = nd->next_sibling( "lees_edwards_portal" ) )
  {
    // Read the first plane index
    const rapidxml::xml_attribute<>* const attrib_a{ nd->first_attribute( "planeA" ) };
    if( !attrib_a )
    {
      std::cerr << "Failed to locate planeA attribute for lees_edwards_portal node." << std::endl;
      return false;
    }
    unsigned index_a;
    if( !StringUtilities::extractFromString( attrib_a->value(), index_a ) )
    {
      std::cerr << "Failed to parse planeA attribute for lees_edwards_portal node with value " << attrib_a->value() << ". Attribute must be an unsigned integer." << std::endl;
      return false;
    }
    if( index_a >= planes.size() )
    {
      std::cerr << "Failed to parse planeA attribute for lees_edwards_portal node with value " << attrib_a->value() << ". Attribute must be an index of a plane between " << 0 << " and " << planes.size() - 1 << std::endl;
      return false;
    }

    // Read the second plane index
    const rapidxml::xml_attribute<>* const attrib_b{ nd->first_attribute( "planeB" ) };
    if( !attrib_b )
    {
      std::cerr << "Failed to locate planeB attribute for lees_edwards_portal node." << std::endl;
      return false;
    }
    unsigned index_b;
    if( !StringUtilities::extractFromString( attrib_b->value(), index_b ) )
    {
      std::cerr << "Failed to parse planeB attribute for lees_edwards_portal node with value " << attrib_b->value() << ". Attribute must be an unsigned integer." << std::endl;
      return false;
    }
    if( index_b >= planes.size() )
    {
      std::cerr << "Failed to parse planeB attribute for lees_edwards_portal node with value " << attrib_b->value() << ". Attribute must be an index of a plane between " << 0 << " and " << planes.size() - 1 << std::endl;
      return false;
    }

    // Indices for this portal can't repeat
    if( index_a == index_b )
    {
      std::cerr << "Failed to parse planeB attribute for lees_edwards_portal node with value " << attrib_b->value() << ". Value is a repeat of attribute planeA." << std::endl;
      return false;
    }

    // Indices can not repeat previous portals
    for( std::vector<std::pair<unsigned,unsigned>>::size_type i = 0; i < plane_pairs.size(); ++i )
    {
      if( index_a == plane_pairs[i].first || index_a == plane_pairs[i].second )
      {
        std::cerr << "Failed to parse planeA attribute for lees_edwards_portal node with value " << attrib_a->value() << ". Plane index is used by an existing portal." << std::endl;
        return false;
      }
      if( index_b == plane_pairs[i].first || index_b == plane_pairs[i].second )
      {
        std::cerr << "Failed to parse planeB attribute for lees_edwards_portal node with value " << attrib_b->value() << ". Plane index is used by an existing portal." << std::endl;
        return false;
      }
    }

    plane_pairs.emplace_back( index_a, index_b );

    // Load the velocity of portal a
    scalar va;
    {
      if( nd->first_attribute( "va" ) == nullptr )
      {
        std::cerr << "Could not locate va attribue for lees_edwards_portal" << std::endl;
        return false;
      }
      const rapidxml::xml_attribute<>& va_attrib{ *nd->first_attribute( "va" ) };
      if( !StringUtilities::extractFromString( std::string{ va_attrib.value() }, va ) )
      {
        std::cerr << "Could not load va attribue for lees_edwards_portal, value must be a scalar" << std::endl;
        return false;
      }
    }

    // Load the velocity of portal b
    scalar vb;
    {
      if( nd->first_attribute( "vb" ) == nullptr )
      {
        std::cerr << "Could not locate vb attribue for lees_edwards_portal" << std::endl;
        return false;
      }
      const rapidxml::xml_attribute<>& vb_attrib{ *nd->first_attribute( "vb" ) };
      if( !StringUtilities::extractFromString( std::string{ vb_attrib.value() }, vb ) )
      {
        std::cerr << "Could not load vb attribue for lees_edwards_portal, value must be a scalar" << std::endl;
        return false;
      }
    }

    // Load the bounds on portal a's translation
    VectorXs boundsa;
    {
      if( nd->first_attribute( "boundsa" ) == nullptr )
      {
        std::cerr << "Could not locate boundsa attribue for lees_edwards_portal" << std::endl;
        return false;
      }
      const rapidxml::xml_attribute<>& boundsa_attrib{ *nd->first_attribute( "boundsa" ) };
      if( !StringUtilities::readScalarList( boundsa_attrib.value(), 2, ' ', boundsa ) )
      {
        std::cerr << "Failed to load boundsa attribute for lees_edwards_portal, must provide 2 scalars" << std::endl;
        return false;
      }
      if( boundsa(0) > 0 )
      {
        std::cerr << "Failed to load boundsa attribute for lees_edwards_portal, first scalar must be non-positive" << std::endl;
        return false;
      }
      if( boundsa(1) < 0 )
      {
        std::cerr << "Failed to load boundsa attribute for lees_edwards_portal, first scalar must be non-negative" << std::endl;
        return false;
      }
      if( !( ( boundsa.x() != -SCALAR_INFINITY && boundsa.y() != SCALAR_INFINITY ) || ( boundsa.x() == -SCALAR_INFINITY && boundsa.y() == SCALAR_INFINITY ) ) )
      {
        std::cerr << "Failed to load boundsa attribute for lees_edwards_portal, if first scalar is negative infinity second scalar must be positive infinity" << std::endl;
        return false;
      }
    }

    // Load the bounds on portal b's translation
    VectorXs boundsb;
    {
      if( nd->first_attribute( "boundsb" ) == nullptr )
      {
        std::cerr << "Could not locate boundsb attribue for lees_edwards_portal" << std::endl;
        return false;
      }
      const rapidxml::xml_attribute<>& boundsb_attrib{ *nd->first_attribute( "boundsb" ) };
      if( !StringUtilities::readScalarList( boundsb_attrib.value(), 2, ' ', boundsb ) )
      {
        std::cerr << "Failed to load boundsb attribute for lees_edwards_portal, must provide 2 scalars" << std::endl;
        return false;
      }
      if( boundsb(0) > 0 )
      {
        std::cerr << "Failed to load boundsb attribute for lees_edwards_portal, first scalar must be non-positive" << std::endl;
        return false;
      }
      if( boundsb(1) < 0 )
      {
        std::cerr << "Failed to load boundsb attribute for lees_edwards_portal, first scalar must be non-negative" << std::endl;
        return false;
      }
      if( !( ( boundsb.x() != -SCALAR_INFINITY && boundsb.y() != SCALAR_INFINITY ) || ( boundsb.x() == -SCALAR_INFINITY && boundsb.y() == SCALAR_INFINITY ) ) )
      {
        std::cerr << "Failed to load boundsb attribute for lees_edwards_portal, if first scalar is negative infinity second scalar must be positive infinity" << std::endl;
        return false;
      }
    }

    plane_tangent_velocities.emplace_back( va, vb );
    plane_bounds.emplace_back( boundsa, boundsb );
  }

  assert( plane_pairs.size() == plane_tangent_velocities.size() );
  assert( plane_pairs.size() == plane_bounds.size() );
  for( std::vector<std::pair<unsigned,unsigned>>::size_type i = 0; i < plane_pairs.size(); ++i )
  {
    planar_portals.emplace_back( planes[plane_pairs[i].first], planes[plane_pairs[i].second], plane_tangent_velocities[i].first, plane_tangent_velocities[i].second, plane_bounds[i].first, plane_bounds[i].second );
  }

  // TODO: This could get slow if there are a ton of portals, but probably not too big of a deal for now
  {
    std::vector<unsigned> indices;
    for( std::vector<std::pair<unsigned,unsigned>>::size_type i = 0; i < plane_pairs.size(); ++i )
    {
      indices.emplace_back( plane_pairs[i].first );
      indices.emplace_back( plane_pairs[i].second );
    }
    std::sort( indices.begin(), indices.end() );
    for( unsigned i = indices.size(); i-- > 0; )
    {
      planes.erase( planes.begin() + indices[i] );
    }
  }

  return true;
}

static bool loadIntegrator( const rapidxml::xml_node<>& node, std::unique_ptr<UnconstrainedMap>& integrator, std::string& dt_string, Rational<std::intmax_t>& dt )
{
  assert( dt_string.empty() );

  // Attempt to locate the integrator node
  const rapidxml::xml_node<>* nd{ node.first_node( "integrator" ) };
  if( nd == nullptr )
  {
    std::cerr << "Failed to locate integrator node." << std::endl;
    return false;
  }

  // Attempt to load the timestep
  {
    const rapidxml::xml_attribute<>* dtnd{ nd->first_attribute( "dt" ) };
    if( dtnd == nullptr )
    {
      std::cerr << "Failed to locate dt attribute for integrator node." << std::endl;
      return false;
    }
    if( !extractFromString( std::string( dtnd->value() ), dt ) || !dt.positive() )
    {
      std::cerr << "Failed to load dt attribute for integrator. Must provide a positive number." << std::endl;
      return false;
    }
    dt_string = dtnd->value();
  }

  // Attempt to load the integrator type
  {
    const rapidxml::xml_attribute<>* typend{ nd->first_attribute( "type" ) };
    if( typend == nullptr )
    {
      std::cerr << "Failed to locate type attribute for integrator node." << std::endl;
      return false;
    }
    const std::string integrator_type{ typend->value() };
    if( integrator_type == "verlet" )
    {
      integrator.reset( new VerletMap );
    }
    else if( integrator_type == "symplectic_euler" )
    {
      integrator.reset( new SymplecticEulerMap );
    }
    else
    {
      std::cerr << "Invalid integrator 'type' attribute specified for integrator node. Options are: verlet, symplectic_euler." << std::endl;
      return false;
    }
  }

  return true;
}

static bool loadLCPSolver( const rapidxml::xml_node<>& node, std::unique_ptr<ImpactOperator>& impact_operator )
{
  const rapidxml::xml_attribute<>* const nd{ node.first_attribute( "name" ) };

  if( nd == nullptr )
  {
    return false;
  }

  const std::string solver_name = std::string{ nd->value() };

  if( solver_name == "ql" )
  {
    // Attempt to parse the solver tolerance
    const rapidxml::xml_attribute<>* const tol_nd{ node.first_attribute( "tol" ) };
    if( tol_nd == nullptr )
    {
      std::cerr << "Could not locate tol for ql solver" << std::endl;
      return false;
    }
    scalar tol;
    if( !StringUtilities::extractFromString( std::string{ tol_nd->value() }, tol ) )
    {
      std::cerr << "Could not load tol for ql solver" << std::endl;
      return false;
    }
    impact_operator.reset( new LCPOperatorQL{ tol } );
  }
  else if( solver_name == "ql_vp" )
  {
    // Attempt to parse the solver tolerance
    const rapidxml::xml_attribute<>* const tol_nd{ node.first_attribute( "tol" ) };
    if( tol_nd == nullptr )
    {
      std::cerr << "Could not locate tol for ql_vp solver" << std::endl;
      return false;
    }
    scalar tol;
    if( !StringUtilities::extractFromString( std::string{ tol_nd->value() }, tol ) )
    {
      std::cerr << "Could not load tol for ql_vp solver" << std::endl;
      return false;
    }
    impact_operator.reset( new LCPOperatorQLVP{ tol } );
  }
  else if( solver_name == "ipopt" )
  {
    // Attempt to read the desired linear solvers
    std::vector<std::string> linear_solvers;
    {
      const rapidxml::xml_attribute<>* const attrib{ node.first_attribute( "linear_solvers" ) };
      if( attrib == nullptr )
      {
        std::cerr << "Could not locate linear solvers for ipopt solver" << std::endl;
        return false;
      }

      std::stringstream ss;
      ss << attrib->value();
      std::string input_string;
      while( ss >> input_string ) { linear_solvers.emplace_back( input_string ); }
      if( linear_solvers.empty() )
      {
        std::cerr << "Could not locate linear solvers for ipopt solver" << std::endl;
        return false;
      }
    }

    // Attempt to read the convergence tolerance
    scalar con_tol = std::numeric_limits<scalar>::signaling_NaN();
    {
      const rapidxml::xml_attribute<>* const attrib{ node.first_attribute( "con_tol" ) };
      if( attrib == nullptr )
      {
        std::cerr << "Could not locate con_tol attribute for ipopt solver" << std::endl;
        return false;
      }

      if( !StringUtilities::extractFromString( attrib->value(), con_tol ) )
      {
        std::cerr << "Could not load con_tol for ipopt solver" << std::endl;
        return false;
      }

      if( con_tol <= 0.0 )
      {
        std::cerr << "Could not load con_tol for ipopt solver, value must be positive scalar." << std::endl;
        return false;
      }
    }
    impact_operator.reset( new LCPOperatorIpopt{ linear_solvers, con_tol } );
  }
  else
  {
    return false;
  }
  
  return true;
}

// TODO: Clean this function up, pull into SCISim
static bool loadImpactOperatorNoCoR( const rapidxml::xml_node<>& node, std::unique_ptr<ImpactOperator>& impact_operator )
{
  // Attempt to load the impact operator type
  std::string type;
  {
    const rapidxml::xml_attribute<>* const typend{ node.first_attribute( "type" ) };
    if( typend == nullptr )
    {
      std::cerr << "Could not locate type" << std::endl;
      return false;
    }
    type = typend->value();
  }

  scalar v_tol = std::numeric_limits<scalar>::signaling_NaN();
  if( type == "gauss_seidel" || type == "jacobi" || type == "gr" )
  {
    // Attempt to load the termination tolerance
    const rapidxml::xml_attribute<>* const v_tol_nd{ node.first_attribute( "v_tol" ) };
    if( v_tol_nd == nullptr )
    {
      std::cerr << "Could not locate v_tol" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( std::string{ v_tol_nd->value() }, v_tol ) || v_tol < 0.0 )
    {
      std::cerr << "Could not load v_tol, value must be a positive scalar" << std::endl;
      return false;
    }
  }

  if( type == "gauss_seidel" )
  {
    impact_operator.reset( new GaussSeidelOperator{ v_tol } );
  }
  else if( type == "jacobi" )
  {
    impact_operator.reset( new JacobiOperator{ v_tol } );
  }
  else if( type == "lcp" )
  {
    if( node.first_node( "solver" ) != nullptr )
    {
      if( !loadLCPSolver( *node.first_node( "solver" ), impact_operator ) )
      {
        return false;
      }
    }
    else
    {
      return false;
    }
  }
  else if( type == "gr" )
  {
    if( node.first_node( "solver" ) != nullptr )
    {
      std::unique_ptr<ImpactOperator> lcp_solver{ nullptr };
      if( !loadLCPSolver( *node.first_node( "solver" ), lcp_solver ) )
      {
        return false;
      }
      impact_operator.reset( new GROperator{ v_tol, *lcp_solver } );
    }
    else
    {
      return false;
    }
  }
  else if( type == "grr" )
  {
    // Generalized restitution requires an elastic operator
    std::unique_ptr<ImpactOperator> elastic_operator{ nullptr };
    {
      const rapidxml::xml_node<>* const elastic_node{ node.first_node( "elastic_operator" ) };
      if( elastic_node == nullptr )
      {
        std::cerr << "Failed to locate elastic_operator for grr impact_operator" << std::endl;
        return false;
      }
      if( !loadImpactOperatorNoCoR( *elastic_node, elastic_operator ) )
      {
        std::cerr << "Failed to load elastic_operator for grr impact_operator" << std::endl;
        return false;
      }
    }

    // Generalized restitution requires an inelastic operator
    std::unique_ptr<ImpactOperator> inelastic_operator{ nullptr };
    {
      const rapidxml::xml_node<>* const inelastic_node{ node.first_node( "inelastic_operator" ) };
      if( inelastic_node == nullptr )
      {
        std::cerr << "Failed to locate inelastic_operator for grr impact_operator" << std::endl;
        return false;
      }
      if( !loadImpactOperatorNoCoR( *inelastic_node, inelastic_operator ) )
      {
        std::cerr << "Failed to load inelastic_operator for grr impact_operator" << std::endl;
        return false;
      }
    }

    impact_operator.reset( new GRROperator{ *elastic_operator, *inelastic_operator } );
  }
  else
  {
    return false;
  }

  return true;
}

static bool loadImpactOperator( const rapidxml::xml_node<>& node, std::unique_ptr<ImpactOperator>& impact_operator, scalar& CoR )
{
  // Attempt to load the CoR
  const rapidxml::xml_attribute<>* const cor_nd{ node.first_attribute( "CoR" ) };
  if( cor_nd == nullptr )
  {
    std::cerr << "Could not locate CoR" << std::endl;
    return false;
  }

  CoR = std::numeric_limits<scalar>::signaling_NaN();
  if( !StringUtilities::extractFromString( std::string{ cor_nd->value() }, CoR ) )
  {
    std::cerr << "Could not load CoR value" << std::endl;
    return false;
  }

  return loadImpactOperatorNoCoR( node, impact_operator );
}

static bool loadQLMDPOperator( const rapidxml::xml_node<>& node, std::unique_ptr<FrictionOperator>& friction_operator )
{
  // Attempt to parse the solver tolerance
  scalar tol;
  {
    const rapidxml::xml_attribute<>* const tol_nd{ node.first_attribute( "tol" ) };
    if( tol_nd == nullptr )
    {
      std::cerr << "Could not locate tol for QL MDP solver" << std::endl;
      return false;
    }
    if( !StringUtilities::extractFromString( std::string{ tol_nd->value() }, tol ) || tol < 0.0 )
    {
      std::cerr << "Could not load tol for QL MDP solver, value must be a positive scalar" << std::endl;
      return false;
    }
  }
  friction_operator.reset( new BoundConstrainedMDPOperatorQL{ tol } );
  return true;
}

static bool loadMDPOperator( const rapidxml::xml_node<>& node, std::unique_ptr<FrictionOperator>& friction_operator )
{
  // Attempt to load the impact operator type
  std::string name;
  {
    const rapidxml::xml_attribute<>* const typend{ node.first_attribute( "name" ) };
    if( typend == nullptr )
    {
      std::cerr << "Could not locate name" << std::endl;
      return false;
    }
    name = typend->value();
  }

  if( name == "ql" )
  {
    return loadQLMDPOperator( node, friction_operator );
  }
  return false;
}

// Example:
//  <staggerd_projections_friction_solver mu="2.0" CoR="0.8" max_iters="50" tol="1.0e-8" staggering="geometric" internal_warm_start_alpha="1" internal_warm_start_beta="1">
//    <lcp_impact_solver name="apgd" tol="1.0e-12" max_iters="5000"/>
//    <mdp_friction_solver name="apgd" tol="1.0e-12" max_iters="5000"/>
//  </staggerd_projections_friction_solver>
static bool loadStaggeredProjectionsFrictionSolver( const rapidxml::xml_node<>& node, scalar& mu, scalar& CoR, std::unique_ptr<FrictionSolver>& friction_solver, std::unique_ptr<ImpactFrictionMap>& if_map )
{
  // Friction solver setup
  {
    // Attempt to load the coefficient of friction
    {
      const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "mu" ) };
      if( attrib_nd == nullptr )
      {
        std::cerr << "Could not locate mu for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      mu = std::numeric_limits<scalar>::signaling_NaN();
      if( !StringUtilities::extractFromString( attrib_nd->value(), mu ) )
      {
        std::cerr << "Could not load mu value for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( mu < 0.0 )
      {
        std::cerr << "Could not load mu value for staggerd_projections_friction_solver, value of mu must be a nonnegative scalar" << std::endl;
        return false;
      }
    }

    // Attempt to load the coefficient of restitution
    {
      const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "CoR" ) };
      if( attrib_nd == nullptr )
      {
        std::cerr << "Could not locate CoR for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      CoR = std::numeric_limits<scalar>::signaling_NaN();
      if( !StringUtilities::extractFromString( attrib_nd->value(), CoR ) )
      {
        std::cerr << "Could not load CoR value for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( CoR < 0.0 || CoR > 1.0 )
      {
        std::cerr << "Could not load CoR value for staggerd_projections_friction_solver, value of CoR must be a nonnegative scalar" << std::endl;
        return false;
      }
    }

    // Attempt to load a warm start alpha setting
    bool internal_warm_start_alpha;
    {
      const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "internal_warm_start_alpha" ) };
      if( attrib_nd == nullptr )
      {
        std::cerr << "Could not locate internal_warm_start_alpha for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( !StringUtilities::extractFromString( attrib_nd->value(), internal_warm_start_alpha ) )
      {
        std::cerr << "Could not load internal_warm_start_alpha value for staggerd_projections_friction_solver, value of internal_warm_start_alpha must be a boolean" << std::endl;
        return false;
      }
    }

    // Attempt to load a warm start beta setting
    bool internal_warm_start_beta;
    {
      const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "internal_warm_start_beta" ) };
      if( attrib_nd == nullptr )
      {
        std::cerr << "Could not locate internal_warm_start_beta for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( !StringUtilities::extractFromString( attrib_nd->value(), internal_warm_start_beta ) )
      {
        std::cerr << "Could not load internal_warm_start_beta value for staggerd_projections_friction_solver, value of internal_warm_start_alpha must be a boolean" << std::endl;
        return false;
      }
    }

    // Attempt to load the impact operator
    std::unique_ptr<ImpactOperator> impact_operator;
    {
      const rapidxml::xml_node<>* const impact_operator_node{ node.first_node( "lcp_impact_solver" ) };
      if( impact_operator_node == nullptr )
      {
        std::cerr << "Could not locate lcp_impact_solver node for staggerd_projections_friction_solver" << std::endl;
        return false;
      }
      if( !loadLCPSolver( *impact_operator_node, impact_operator ) )
      {
        return false;
      }
    }

    // Attempt to load the friction operator
    std::unique_ptr<FrictionOperator> friction_operator;
    {
      const rapidxml::xml_node<>* const friction_operator_node{ node.first_node( "mdp_friction_solver" ) };
      if( friction_operator_node == nullptr )
      {
        std::cerr << "Could not locate mdp_friction_solver node for staggerd_projections_friction_solver" << std::endl;
        return false;
      }
      if( !loadMDPOperator( *friction_operator_node, friction_operator ) )
      {
        return false;
      }
    }

    friction_solver.reset( new StaggeredProjections( internal_warm_start_alpha, internal_warm_start_beta, *impact_operator, *friction_operator ) );
  }

  // Impact-friction map setup
  // TODO: setting up if_map can be done in a separate function
  {
    // Attempt to load the staggering type
    std::string staggering_type;
    {
      const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "staggering" ) };
      if( attrib_nd == nullptr )
      {
        std::cerr << "Could not locate staggering attribute for staggerd_projections_friction_solver" << std::endl;
        return false;
      }
      staggering_type = attrib_nd->value();
    }

    // Attempt to load the termination tolerance
    scalar tol;
    {
      const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "tol" ) };
      if( attrib_nd == nullptr )
      {
        std::cerr << "Could not locate tol for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( !StringUtilities::extractFromString( attrib_nd->value(), tol ) )
      {
        std::cerr << "Could not load tol value for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( tol < 0.0 )
      {
        std::cerr << "Could not load tol value for staggerd_projections_friction_solver, value of tol must be a nonnegative scalar" << std::endl;
        return false;
      }
    }

    // Attempt to load the maximum number of iterations
    int max_iters;
    {
      const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "max_iters" ) };
      if( attrib_nd == nullptr )
      {
        std::cerr << "Could not locate max_iters for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( !StringUtilities::extractFromString( attrib_nd->value(), max_iters ) )
      {
        std::cerr << "Could not load max_iters value for staggerd_projections_friction_solver" << std::endl;
        return false;
      }

      if( max_iters <= 0 )
      {
        std::cerr << "Could not load max_iters value for staggerd_projections_friction_solver, value of max_iters must be positive integer." << std::endl;
        return false;
      }
    }

    if( staggering_type == "geometric" )
    {
      if_map.reset( new GeometricImpactFrictionMap{ tol, static_cast<unsigned>( max_iters ), false, false } );
    }
    else if( staggering_type == "stabilized" )
    {
      if_map.reset( new StabilizedImpactFrictionMap{ tol, static_cast<unsigned>( max_iters ) } );
    }
    else
    {
      std::cerr << "Invalid staggering attribute specified for staggerd_projections_friction_solver, options are: ";
      std::cerr << "geometric, stabilized" << std::endl;
      return false;
    }
  }

  return true;
}

static bool loadSobogusFrictionSolver( const rapidxml::xml_node<>& node, std::unique_ptr<FrictionSolver>& friction_solver, scalar& mu, scalar& CoR, std::unique_ptr<ImpactFrictionMap>& if_map )
{
  // Attempt to load the coefficient of friction
  {
    const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "mu" ) };
    if( attrib_nd == nullptr )
    {
      std::cerr << "Could not locate mu for sobogus_friction_solver" << std::endl;
      return false;
    }

    mu = std::numeric_limits<scalar>::signaling_NaN();
    if( !StringUtilities::extractFromString( attrib_nd->value(), mu ) )
    {
      std::cerr << "Could not load mu value for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( mu < 0.0 )
    {
      std::cerr << "Could not load mu value for sobogus_friction_solver, value of mu must be a nonnegative scalar" << std::endl;
      return false;
    }
  }

  // Attempt to load the coefficient of restitution
  {
    const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "CoR" ) };
    if( attrib_nd == nullptr )
    {
      std::cerr << "Could not locate CoR for sobogus_friction_solver" << std::endl;
      return false;
    }

    CoR = std::numeric_limits<scalar>::signaling_NaN();
    if( !StringUtilities::extractFromString( attrib_nd->value(), CoR ) )
    {
      std::cerr << "Could not load CoR value for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( CoR < 0.0 || CoR > 1.0 )
    {
      std::cerr << "Could not load CoR value for sobogus_friction_solver, value of CoR must be a nonnegative scalar" << std::endl;
      return false;
    }
  }

  // Attempt to load the maximum number of iterations
  int max_iters;
  {
    const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "max_iters" ) };
    if( attrib_nd == nullptr )
    {
      std::cerr << "Could not locate max_iters for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( !StringUtilities::extractFromString( attrib_nd->value(), max_iters ) )
    {
      std::cerr << "Could not load max_iters value for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( max_iters <= 0 )
    {
      std::cerr << "Could not load max_iters value for sobogus_friction_solver, value of max_iters must be positive integer." << std::endl;
      return false;
    }
  }

  // Attempt to load the spacing between error evaluation
  int eval_every;
  {
    const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "eval_every" ) };
    if( attrib_nd == nullptr )
    {
      std::cerr << "Could not locate eval_every for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( !StringUtilities::extractFromString( attrib_nd->value(), eval_every ) )
    {
      std::cerr << "Could not load eval_every value for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( eval_every <= 0 )
    {
      std::cerr << "Could not load eval_every value for sobogus_friction_solver, value of max_iters must be positive integer." << std::endl;
      return false;
    }

    if( eval_every > max_iters )
    {
      std::cerr << "Could not load eval_every value for sobogus_friction_solver, value of eval_every must must be less than or equal to max_iters." << std::endl;
      return false;
    }
  }

  // Attempt to load the termination tolerance
  scalar tol;
  {
    const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "tol" ) };
    if( attrib_nd == nullptr )
    {
      std::cerr << "Could not locate tol for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( !StringUtilities::extractFromString( attrib_nd->value(), tol ) )
    {
      std::cerr << "Could not load tol value for sobogus_friction_solver" << std::endl;
      return false;
    }

    if( tol < 0.0 )
    {
      std::cerr << "Could not load tol value for sobogus_friction_solver, value of tol must be a nonnegative scalar" << std::endl;
      return false;
    }
  }

  // Attempt to load the staggering type
  std::string staggering_type;
  {
    const rapidxml::xml_attribute<>* const attrib_nd{ node.first_attribute( "staggering" ) };
    if( attrib_nd == nullptr )
    {
      std::cerr << "Could not locate staggering attribute for sobogus_friction_solver" << std::endl;
      return false;
    }
    staggering_type = attrib_nd->value();
  }

  if( staggering_type == "geometric" )
  {
    if_map.reset( new GeometricImpactFrictionMap{ tol, static_cast<unsigned>( max_iters ), false, false } );
  }
  else if( staggering_type == "stabilized" )
  {
    if_map.reset( new StabilizedImpactFrictionMap{ tol, static_cast<unsigned>( max_iters ) } );
  }
  else
  {
    std::cerr << "Invalid staggering attribute specified for sobogus_friction_solver" << std::endl;
    return false;
  }

  friction_solver.reset( new Sobogus{ SobogusSolverType::Balls2D, static_cast<unsigned>( eval_every ) } );

  return true;
}

static bool loadGravityForce( const rapidxml::xml_node<>& node, std::vector<std::unique_ptr<Ball2DForce>>& forces )
{
  for( rapidxml::xml_node<>* nd = node.first_node( "gravity" ); nd; nd = nd->next_sibling( "gravity" ) )
  {
    Vector2s f;

    const rapidxml::xml_attribute<>* x_attrib{ nd->first_attribute( "fx" ) };
    if( !x_attrib ) { return false; }
    StringUtilities::extractFromString( x_attrib->value(), f.x() );

    const rapidxml::xml_attribute<>* y_attrib{ nd->first_attribute( "fy" ) };
    if( !y_attrib ) { return false; }
    StringUtilities::extractFromString( y_attrib->value(), f.y() );

    forces.emplace_back( new Ball2DGravityForce{ f } );
  }

  return true;
}

static bool loadHertzianPenaltyForce( const rapidxml::xml_node<>& node, std::vector<std::unique_ptr<Ball2DForce>>& forces )
{
  for( rapidxml::xml_node<>* nd = node.first_node( "hertzian_penalty" ); nd; nd = nd->next_sibling( "hertzian_penalty" ) )
  {
    const rapidxml::xml_attribute<>* k_attrib{ nd->first_attribute( "k" ) };
    if( !k_attrib )
    {
      std::cerr << "Failed to locate k attribute for hertzian_penalty." << std::endl;
      return false;
    }
    scalar k;
    if( !StringUtilities::extractFromString( k_attrib->value(), k ) || k < 0.0 )
    {
      std::cerr << "Failed to load k attribute for hertzian_penalty. Value must be a positive scalar." << std::endl;
      return false;
    }
    
    forces.emplace_back( new HertzianPenaltyForce{ k } );
  }
  
  return true;
}

bool XMLSceneParser::parseXMLSceneFile( const std::string& file_name, std::string& scripting_callback_name, std::vector<Ball2D>& balls, std::vector<StaticDrum>& drums, std::vector<StaticPlane>& planes, std::vector<PlanarPortal>& planar_portals, std::unique_ptr<UnconstrainedMap>& integrator, std::string& dt_string, Rational<std::intmax_t>& dt, scalar& end_time, std::unique_ptr<ImpactOperator>& impact_operator, std::unique_ptr<ImpactMap>& impact_map, scalar& CoR, std::unique_ptr<FrictionSolver>& friction_solver, scalar& mu, std::unique_ptr<ImpactFrictionMap>& if_map, std::vector<std::unique_ptr<Ball2DForce>>& forces, bool& camera_set, Eigen::Vector2d& camera_center, double& camera_scale_factor, unsigned& fps, bool& render_at_fps, bool& lock_camera )
{
  // Attempt to load the xml document
  std::vector<char> xmlchars;
  rapidxml::xml_document<> doc;
  if( !loadXMLFile( file_name, xmlchars, doc ) )
  {
    return false;
  }

  // Attempt to locate the root node
  if( doc.first_node( "ball2d_scene" ) == nullptr )
  {
    std::cerr << "Failed to locate root node in xml scene file: " << file_name << std::endl;
    return false;
  }
  const rapidxml::xml_node<>& root_node{ *doc.first_node( "ball2d_scene" ) };

  // Attempt to determine if scirpting is enabled and if so, the coresponding callback
  if( !loadScriptingSetup( root_node, scripting_callback_name ) )
  {
    std::cerr << "Failed to parse scripting node in xml scene file: " << file_name << std::endl;
    return false;
  }

  // Attempt to load the end time, if present
  end_time = SCALAR_INFINITY;
  if( root_node.first_node( "end_time" ) != nullptr )
  {
    if( !loadEndTime( *root_node.first_node( "end_time" ), end_time ) )
    {
      std::cerr << "Failed to parse end_time node: " << file_name << std::endl;
      return false;
    }
  }

  // Attempt to load a gravity force
  if( !loadGravityForce( root_node, forces ) )
  {
    std::cerr << "Failed to load gravity force: " << file_name << std::endl;
    return false;
  }

  // Attempt to load a hertzian penalty force
  if( !loadHertzianPenaltyForce( root_node, forces ) )
  {
    std::cerr << "Failed to load hertzian penalty force: " << file_name << std::endl;
    return false;
  }

  // Attempt to load camera settings, if present
  camera_set = false;
  if( root_node.first_node( "camera" ) != nullptr )
  {
    camera_set = true;
    if( !loadCameraSettings( *root_node.first_node( "camera" ), camera_center, camera_scale_factor, fps, render_at_fps, lock_camera ) )
    {
      std::cerr << "Failed to parse camera node: " << file_name << std::endl;
      return false;
    }
  }

  // Attempt to load the unconstrained integrator
  if( !loadIntegrator( root_node, integrator, dt_string, dt ) )
  {
    std::cerr << "Failed to load integrator: " << file_name << std::endl;
    return false;
  }

  // Attempt to load an impact operator
  if( root_node.first_node( "impact_operator" ) != nullptr )
  {
    if( !loadImpactOperator( *root_node.first_node( "impact_operator" ), impact_operator, CoR ) )
    {
      std::cerr << "Failed to load impact_operator in xml scene file: " << file_name << std::endl;
      return false;
    }
    impact_map.reset( new ImpactMap{ false } );
  }
  else
  {
    impact_operator.reset( nullptr );
    impact_map.reset( nullptr );
    CoR = SCALAR_NAN;
  }

  friction_solver.reset( nullptr );
  mu = SCALAR_NAN;
  if_map.reset( nullptr );

  // Load a staggered projections friction solver, if present
  if( root_node.first_node( "staggerd_projections_friction_solver" ) != nullptr )
  {
    if( impact_operator != nullptr || impact_map != nullptr )
    {
      std::cerr << "Error loading staggerd_projections_friction_solver, solver of type " << impact_operator->name() << " already specified" << std::endl;
      return false;
    }
    if( friction_solver != nullptr )
    {
      std::cerr << "Error loading staggerd_projections_friction_solver, solver of type " << friction_solver->name() << " already specified" << std::endl;
      return false;
    }
    if( !loadStaggeredProjectionsFrictionSolver( *root_node.first_node( "staggerd_projections_friction_solver" ), mu, CoR, friction_solver, if_map ) )
    {
      std::cerr << "Failed to load staggerd_projections_friction_solver in xml scene file: " << file_name << std::endl;
      return false;
    }
  }

  // Load a Sobogus friction solver, if present
  if( root_node.first_node( "sobogus_friction_solver" ) != nullptr )
  {
    if( impact_operator != nullptr || impact_map != nullptr )
    {
      std::cerr << "Error loading sobogus_friction_solver, solver of type " << impact_operator->name() << " already specified" << std::endl;
      return false;
    }
    if( friction_solver != nullptr )
    {
      std::cerr << "Error loading sobogus_friction_solver, solver of type " << friction_solver->name() << " already specified" << std::endl;
      return false;
    }
    if( !loadSobogusFrictionSolver( *root_node.first_node( "sobogus_friction_solver" ), friction_solver, mu, CoR, if_map ) )
    {
      std::cerr << "Failed to load sobogus_friction_solver in xml scene file: " << file_name << std::endl;
      return false;
    }
  }

  // TODO: GRR friction solver goes here

  // Attempt to load any user-provided static drums
  if( !loadStaticDrums( root_node, drums ) )
  {
    std::cerr << "Failed to load static drums: " << file_name << std::endl;
    return false;
  }

  // Attempt to load any user-provided static drums
  if( !loadStaticPlanes( root_node, planes ) )
  {
    std::cerr << "Failed to load static planes: " << file_name << std::endl;
    return false;
  }

  // Attempt to load planar portals
  if( !loadPlanarPortals( root_node, planes, planar_portals ) )
  {
    std::cerr << "Failed to load planar and lees edwards portals: " << file_name << std::endl;
    return false;
  }

  // Attempt to load any user-provided balls
  if( !loadBalls( root_node, balls ) )
  {
    std::cerr << "Failed to load balls: " << file_name << std::endl;
    return false;
  }

  return true;
}