// rigidbody2d_cli.cpp
//
// Breannan Smith
// Last updated: 09/13/2015

#include <iostream>
#include <iomanip>
#include <fstream>
#include <getopt.h>

#include "SCISim/Math/MathDefines.h"
#include "SCISim/Math/MathUtilities.h"
#include "SCISim/Math/Rational.h"
#include "SCISim/StringUtilities.h"
#include "SCISim/CompileDefinitions.h"
#include "SCISim/ConstrainedMaps/ImpactFrictionMap.h"
#include "SCISim/ConstrainedMaps/ImpactMaps/ImpactSolution.h"
#include "SCISim/Timer/TimeUtils.h"
#include "SCISim/ConstrainedMaps/ConstrainedMapUtilities.h"
#include "SCISim/UnconstrainedMaps/UnconstrainedMap.h"
#include "SCISim/ConstrainedMaps/ImpactMaps/ImpactMap.h"
#include "SCISim/ConstrainedMaps/ImpactMaps/ImpactOperator.h"
#include "SCISim/ConstrainedMaps/FrictionSolver.h"
#include "SCISim/HDF5File.h"
#include "SCISim/Utilities.h"

#include "RigidBody2D/RigidBody2DSim.h"
#include "RigidBody2D/RigidBody2DUtilities.h"

#include "RigidBody2DUtils/RigidBody2DSceneParser.h"
#include "RigidBody2DUtils/CameraSettings2D.h"

static RigidBody2DSim g_sim;
static unsigned g_iteration = 0;
static std::unique_ptr<UnconstrainedMap> g_unconstrained_map{ nullptr };
static Rational<std::intmax_t> g_dt;
static scalar g_end_time = SCALAR_NAN;
static std::unique_ptr<ImpactOperator> g_impact_operator{ nullptr };
static scalar g_CoR = SCALAR_NAN;
static std::unique_ptr<FrictionSolver> g_friction_solver{ nullptr };
static scalar g_mu = SCALAR_NAN;
static std::unique_ptr<ImpactMap> g_impact_map{ nullptr };
static std::unique_ptr<ImpactFrictionMap> g_impact_friction_map{ nullptr };
//static std::unique_ptr<ScriptingCallbackBalls2D> g_scripting_callback{ nullptr };

static std::string g_output_dir_name;
static bool g_output_forces{ false };
// Number of timesteps between saves
static unsigned g_steps_per_save{ 0 };
// Number of saves that been conducted so far
static unsigned g_output_frame{ 0 };
static unsigned g_dt_string_precision{ 0 };
static unsigned g_save_number_width{ 0 };

static bool g_serialize_snapshots{ false };
static bool g_overwrite_snapshots{ true };

// Magic number to print in front of binary output to aid in debugging
static const unsigned MAGIC_BINARY_NUMBER{ 8675309 };

static std::string generateOutputConfigurationDataFileName( const std::string& prefix, const std::string& extension )
{
  std::stringstream ss;
  if( !g_output_dir_name.empty() )
  {
    ss << g_output_dir_name << "/";
  }
  ss << prefix << "_" << std::setfill('0') << std::setw( g_save_number_width ) << g_output_frame << "." << extension;
  return ss.str();
}

static void printCompileInfo( std::ostream& output_stream )
{
  output_stream << "Git Revision:     " << CompileDefinitions::GitSHA1 << std::endl;
  output_stream << "Build Mode:       " << CompileDefinitions::BuildMode << std::endl;
  output_stream << "C Compiler:       " << CompileDefinitions::CCompiler << std::endl;
  output_stream << "CXX Compiler:     " << CompileDefinitions::CXXCompiler << std::endl;
  output_stream << "Fortran Compiler: " << CompileDefinitions::FortranCompiler << std::endl;
}

static unsigned computeTimestepDisplayPrecision( const Rational<std::intmax_t>& dt, const std::string& dt_string )
{
  if( dt_string.find( '.' ) != std::string::npos )
  {
    return unsigned( StringUtilities::computeNumCharactersToRight( dt_string, '.' ) );
  }
  else
  {
    std::string converted_dt_string;
    std::stringstream ss;
    ss << std::fixed << scalar( dt );
    ss >> converted_dt_string;
    return unsigned( StringUtilities::computeNumCharactersToRight( converted_dt_string, '.' ) );
  }
}

static bool loadXMLScene( const std::string& xml_file_name )
{
  std::string scripting_callback_name;
  std::string dt_string;
  CameraSettings2D unused_camera_settings;
  RigidBody2DState state;

  const bool loaded_successfully{ RigidBody2DSceneParser::parseXMLSceneFile( xml_file_name, scripting_callback_name, state, g_unconstrained_map, dt_string, g_dt, g_end_time, g_impact_operator, g_impact_map, g_CoR, g_friction_solver, g_mu, g_impact_friction_map, unused_camera_settings ) };

  if( !loaded_successfully )
  {
    return false;
  }

  g_sim = RigidBody2DSim{ state };
  g_dt_string_precision = computeTimestepDisplayPrecision( g_dt, dt_string );
  //g_scripting_callback = KinematicScripting::initializeScriptingCallback( scripting_callback_name );
  //assert( g_scripting_callback != nullptr );

  return true;
}

static std::string generateSimulationTimeString()
{
  std::stringstream time_stream;
  time_stream << std::fixed << std::setprecision( g_dt_string_precision ) << g_iteration * scalar( g_dt );
  return time_stream.str();
}

static int saveState()
{
  // Generate a base filename
  const std::string output_file_name{ generateOutputConfigurationDataFileName( "config", "h5" ) };

  // Print a status message with the simulation time and output number
  std::cout << "Saving state at time " << generateSimulationTimeString() << " to " << output_file_name;
  std::cout << "        " << TimeUtils::currentTime() << std::endl;

  // Save the simulation state
  try
  {
    HDF5File output_file( output_file_name, HDF5File::READ_WRITE );
    // Save the iteration and time step and time
    output_file.writeScalar( "", "timestep", scalar( g_dt ) );
    output_file.writeScalar( "", "iteration", g_iteration );
    output_file.writeScalar( "", "time", scalar( g_dt ) * g_iteration );
    // Save out the git hash
    output_file.writeString( "", "git_hash", CompileDefinitions::GitSHA1 );
    // Save the real time
    //output_file.createGroup( "/run_stats" );
    //output_file.writeString( "/run_stats", "real_time", TimeUtils::currentTime() );
    // Write out the simulation data
    g_sim.writeBinaryState( output_file );
  }
  catch( const std::string& error )
  {
    std::cerr << error << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

static int serializeSystem()
{
  // Generate a base filename
  const std::string serialized_file_name{ g_overwrite_snapshots ? "serial.bin" : generateOutputConfigurationDataFileName( "serial", "bin" ) };

  // Print a message to the user that the state is being written
  std::cout << "Serializing: " << generateSimulationTimeString() << " to " << serialized_file_name;
  std::cout << "        " << TimeUtils::currentTime() << std::endl;

  // Attempt to open the output file
  std::ofstream serial_stream( serialized_file_name, std::ios::binary );
  if( !serial_stream.is_open() )
  {
    std::cerr << "Failed to open serialization file: " << serialized_file_name << std::endl;
    std::cerr << "Exiting." << std::endl;
    return EXIT_FAILURE;
  }

  // Write the magic number
  Utilities::serializeBuiltInType( MAGIC_BINARY_NUMBER, serial_stream );

  // Write the git revision
  {
    const std::string git_revision{ CompileDefinitions::GitSHA1 };
    StringUtilities::serializeString( git_revision, serial_stream );
  }

  // Write the actual state
  g_sim.serialize( serial_stream );
  Utilities::serializeBuiltInType( g_iteration, serial_stream );
  RigidBody2DUtilities::serialize( g_unconstrained_map, serial_stream );
  Utilities::serializeBuiltInType( g_dt, serial_stream );
  Utilities::serializeBuiltInType( g_end_time, serial_stream );
  ConstrainedMapUtilities::serialize( g_impact_operator, serial_stream );
  Utilities::serializeBuiltInType( g_CoR, serial_stream );
  ConstrainedMapUtilities::serialize( g_friction_solver, serial_stream );
  Utilities::serializeBuiltInType( g_mu, serial_stream );
  ConstrainedMapUtilities::serialize( g_impact_map, serial_stream );
  ConstrainedMapUtilities::serialize( g_impact_friction_map, serial_stream );
  //KinematicScripting::serialize( *g_scripting_callback, serial_stream );
  StringUtilities::serializeString( g_output_dir_name, serial_stream );
  Utilities::serializeBuiltInType( g_output_forces, serial_stream );
  Utilities::serializeBuiltInType( g_steps_per_save, serial_stream );
  Utilities::serializeBuiltInType( g_output_frame, serial_stream );
  Utilities::serializeBuiltInType( g_dt_string_precision, serial_stream );
  Utilities::serializeBuiltInType( g_save_number_width, serial_stream );
  Utilities::serializeBuiltInType( g_serialize_snapshots, serial_stream );
  Utilities::serializeBuiltInType( g_overwrite_snapshots, serial_stream );

  return EXIT_SUCCESS;
}

static int deserializeSystem( const std::string& file_name )
{
  std::cout << "Loading serialized simulation state file: " << file_name << std::endl;

  // Attempt to open the input file
  std::ifstream serial_stream{ file_name, std::ios::binary };
  if( !serial_stream.is_open() )
  {
    std::cerr << "Failed to open serialized state in file: " << file_name << std::endl;
    std::cerr << "Exiting." << std::endl;
    return EXIT_FAILURE;
  }

  // Verify the magic number
  if( Utilities::deserialize<unsigned>( serial_stream ) != MAGIC_BINARY_NUMBER )
  {
    std::cerr << "File " << file_name << " does not appear to be a serialized 2D SCISim rigid body simulation. Exiting." << std::endl;
    return EXIT_FAILURE;
  }

  // Read the git revision
  {
    const std::string git_revision{ StringUtilities::deserializeString( serial_stream ) };
    if( CompileDefinitions::GitSHA1 != git_revision )
    {
      std::cerr << "Warning, resuming from data file for a different git revision." << std::endl;
      std::cerr << "   Serialized Git Revision: " << git_revision << std::endl;
      std::cerr << "      Current Git Revision: " << CompileDefinitions::GitSHA1 << std::endl;
    }
    std::cout << "Git Revision: " << git_revision << std::endl;
  }

  g_sim.deserialize( serial_stream );
  g_iteration = Utilities::deserialize<unsigned>( serial_stream );
  g_unconstrained_map = RigidBody2DUtilities::deserializeUnconstrainedMap( serial_stream );
  deserialize( g_dt, serial_stream );
  assert( g_dt.positive() );
  g_end_time = Utilities::deserialize<scalar>( serial_stream );
  assert( g_end_time > 0.0 );
  g_impact_operator = ConstrainedMapUtilities::deserializeImpactOperator( serial_stream );
  g_CoR = Utilities::deserialize<scalar>( serial_stream );
  assert( std::isnan(g_CoR) || g_CoR >= 0.0 ); assert( std::isnan(g_CoR) || g_CoR <= 1.0 );
  g_friction_solver = ConstrainedMapUtilities::deserializeFrictionSolver( serial_stream );
  g_mu = Utilities::deserialize<scalar>( serial_stream );
  assert( std::isnan(g_mu) || g_mu >= 0.0 );
  g_impact_map = ConstrainedMapUtilities::deserializeImpactMap( serial_stream );
  g_impact_friction_map = ConstrainedMapUtilities::deserializeImpactFrictionMap( serial_stream );
  //g_scripting_callback = KinematicScripting::deserializeScriptingCallback( serial_stream );
  g_output_dir_name = StringUtilities::deserializeString( serial_stream );
  g_output_forces = Utilities::deserialize<bool>( serial_stream );
  g_steps_per_save = Utilities::deserialize<unsigned>( serial_stream );
  g_output_frame = Utilities::deserialize<unsigned>( serial_stream );
  g_dt_string_precision = Utilities::deserialize<unsigned>( serial_stream );
  g_save_number_width = Utilities::deserialize<unsigned>( serial_stream );
  g_serialize_snapshots = Utilities::deserialize<bool>( serial_stream );
  g_overwrite_snapshots = Utilities::deserialize<bool>( serial_stream );

  serial_stream.close();

  return EXIT_SUCCESS;
}

static int exportConfigurationData()
{
  assert( g_steps_per_save != 0 );
  if( g_iteration % g_steps_per_save == 0 )
  {
    if( !g_output_dir_name.empty() )
    {
      if( saveState() == EXIT_FAILURE )
      {
        return EXIT_FAILURE;
      }
    }
    if( g_serialize_snapshots )
    {
      if( serializeSystem() == EXIT_FAILURE )
      {
        return EXIT_FAILURE;
      }
    }
    ++g_output_frame;
  }
  return EXIT_SUCCESS;
}

static std::string generateOutputConstraintForceDataFileName()
{
  std::stringstream ss;
  assert( g_output_frame > 0 );
  ss << g_output_dir_name << "/forces_" << std::setfill('0') << std::setw( g_save_number_width ) << g_output_frame - 1 << ".h5";
  return ss.str();
}

static int stepSystem()
{
  const unsigned next_iter{ g_iteration + 1 };

  HDF5File force_file;
  assert( g_steps_per_save != 0 );
  if( g_output_forces && g_iteration % g_steps_per_save == 0 )
  {
    assert( !g_output_dir_name.empty() );
    const std::string constraint_force_file_name{ generateOutputConstraintForceDataFileName() };
    std::cout << "Saving forces at time " << generateSimulationTimeString() << " to " << constraint_force_file_name << std::endl;
    try
    {
      force_file.open( constraint_force_file_name, HDF5File::READ_WRITE );
      // Save the iteration and time step and time
      force_file.writeScalar( "", "timestep", scalar( g_dt ) );
      force_file.writeScalar( "", "iteration", g_iteration );
      force_file.writeScalar( "", "time", scalar( g_dt ) * g_iteration );
      // Save out the git hash
      force_file.writeString( "", "git_hash", CompileDefinitions::GitSHA1 );
      // Save the real time
      //force_file.createGroup( "/run_stats" );
      //force_file.writeString( "/run_stats", "real_time", TimeUtils::currentTime() );
    }
    catch( const std::string& error )
    {
      std::cerr << error << std::endl;
      return EXIT_FAILURE;
    }
  }

  //assert( g_scripting_callback != nullptr );
  //g_scripting_callback->setSimulationState( g_sim.state() );
  //g_scripting_callback->startOfStepCallback( next_iter, g_dt );

  if( g_unconstrained_map == nullptr && g_impact_operator == nullptr && g_impact_map == nullptr && g_friction_solver == nullptr && g_impact_friction_map == nullptr )
  {
    // Nothing to do
  }
  else if( g_unconstrained_map != nullptr && g_impact_operator == nullptr && g_impact_map == nullptr && g_friction_solver == nullptr && g_impact_friction_map == nullptr )
  {
    g_sim.flow( next_iter, scalar( g_dt ), *g_unconstrained_map );
  }
  else if( g_unconstrained_map != nullptr && g_impact_operator != nullptr && g_impact_map != nullptr && g_friction_solver == nullptr && g_impact_friction_map == nullptr )
  {
    assert( g_impact_map != nullptr );
    ImpactSolution impact_solution;
    if( force_file.is_open() )
    {
      g_impact_map->exportForcesNextStep( impact_solution );
    }
    //assert( g_scripting_callback != nullptr );
    g_sim.flow( next_iter, scalar( g_dt ), *g_unconstrained_map, *g_impact_operator, g_CoR, *g_impact_map );
    if( force_file.is_open() )
    {
      try
      {
        impact_solution.writeSolution( force_file );
      }
      catch( const std::string& error )
      {
        std::cerr << error << std::endl;
        return EXIT_FAILURE;
      }
    }
  }
  else if( g_unconstrained_map != nullptr && g_impact_operator == nullptr && g_impact_map == nullptr && g_friction_solver != nullptr && g_impact_friction_map != nullptr )
  {
    if( force_file.is_open() )
    {
      g_impact_friction_map->exportForcesNextStep( force_file );
    }
    g_sim.flow( next_iter, scalar( g_dt ), *g_unconstrained_map, g_CoR, g_mu, *g_friction_solver, *g_impact_friction_map );
  }
  else
  {
    std::cerr << "Impossible code path hit in stepSystem. This is a bug. Exiting." << std::endl;
    return EXIT_FAILURE;
  }

  ++g_iteration;

  return exportConfigurationData();
}

static int executeSimLoop()
{
  if( exportConfigurationData() == EXIT_FAILURE )
  {
    return EXIT_FAILURE;
  }

  while( true )
  {
    // N.B. this will ocassionaly not trigger at the *exact* equal time due to floating point errors
    if( g_iteration * scalar( g_dt ) >= g_end_time )
    {
      // Take one final step to ensure we have force data for end time
      if( g_output_forces )
      {
        if( stepSystem() == EXIT_FAILURE )
        {
          return EXIT_FAILURE;
        }
      }
      std::cout << "Simulation complete at time " << g_iteration * scalar( g_dt ) << ". Exiting." << std::endl;
      return EXIT_SUCCESS;
    }

    if( stepSystem() == EXIT_FAILURE )
    {
      return EXIT_FAILURE;
    }
  }
}

static void printUsage( const std::string& executable_name )
{
  std::cout << "Usage: " << executable_name << " xml_scene_file_name [options]" << std::endl;
  std::cout << "Options are:" << std::endl;
  std::cout << "   -h/--help                : prints this help message and exits" << std::endl;
  std::cout << "   -i/--impulses            : saves impulses in addition to configuration if an output directory is set" << std::endl;
  std::cout << "   -r/--resume file         : resumes the simulation from a serialized file" << std::endl;
  std::cout << "   -e/--end scalar          : overrides the end time specified in the scene file" << std::endl;
  std::cout << "   -o/--output_dir dir      : saves simulation state to the given directory" << std::endl;
  std::cout << "   -f/--frequency integer   : rate at which to save simulation data, in Hz; ignored if no output directory specified" << std::endl;
  std::cout << "   -s/--serialize_snapshots bool : save a bit identical, resumable snapshot; if 0 overwrites the snapshot each timestep, if 1 saves a new snapshot for each timestep" << std::endl;
}

static bool parseCommandLineOptions( int* argc, char*** argv, bool& help_mode_enabled, scalar& end_time_override, unsigned& output_frequency, std::string& serialized_file_name )
{
  const struct option long_options[] =
  {
    { "help", no_argument, 0, 'h' },
    { "impulses", no_argument, 0, 'i' },
    { "serialize_snapshots", required_argument, 0, 's' },
    { "resume", required_argument, 0, 'r' },
    { "end", required_argument, 0, 'e' },
    { "output_dir", required_argument, 0, 'o' },
    { "frequency", required_argument, 0, 'f' },
    { 0, 0, 0, 0 }
  };

  while( true )
  {
    int option_index = 0;
    const int c{ getopt_long( *argc, *argv, "his:r:e:o:f:", long_options, &option_index ) };
    if( c == -1 )
    {
      break;
    }
    switch( c )
    {
      case 'h':
      {
        help_mode_enabled = true;
        break;
      }
      case 'i':
      {
        g_output_forces = true;
        break;
      }
      case 's':
      {
        g_serialize_snapshots = true;
        if( !StringUtilities::extractFromString( optarg, g_overwrite_snapshots ) )
        {
          std::cerr << "Failed to read value for argument for -s/--serialize_snapshots. Value must be a boolean." << std::endl;
          return false;
        }
        g_overwrite_snapshots = !g_overwrite_snapshots;
        break;
      }
      case 'r':
      {
        serialized_file_name = optarg;
        break;
      }
      case 'e':
      {
        if( !StringUtilities::extractFromString( optarg, end_time_override ) )
        {
          std::cerr << "Failed to read value for argument for -e/--end. Value must be a positive scalar." << std::endl;
          return false;
        }
        if( end_time_override <= 0 )
        {
          std::cerr << "Failed to read value for argument for -e/--end. Value must be a positive scalar." << std::endl;
          return false;
        }
        break;
      }
      case 'o':
      {
        g_output_dir_name = optarg;
        break;
      }
      case 'f':
      {
        if( !StringUtilities::extractFromString( optarg, output_frequency ) )
        {
          std::cerr << "Failed to read value for argument for -f/--frequency. Value must be an unsigned integer." << std::endl;
          return false;
        }
        break;
      }
      case '?':
      {
        return false;
      }
      default:
      {
        std::cerr << "This is a bug in the command line parser. Please file a report." << std::endl;
        return false;
      }
    }
  }

  return true;
}

int main( int argc, char** argv )
{
  // Command line options
  bool help_mode_enabled{ false };
  scalar end_time_override{ -1 };
  unsigned output_frequency{ 0 };
  std::string serialized_file_name;

  // Attempt to load command line options
  if( !parseCommandLineOptions( &argc, &argv, help_mode_enabled, end_time_override, output_frequency, serialized_file_name ) )
  {
    return EXIT_FAILURE;
  }

  // If the user requested help, print help and exit
  if( help_mode_enabled )
  {
    printUsage( argv[0] );
    return EXIT_SUCCESS;
  }

  // Check for impossible combinations of options
  if( g_output_forces && g_output_dir_name.empty() )
  {
    std::cerr << "Impulse output requires an output directory." << std::endl;
    return EXIT_FAILURE;
  }

  if( !serialized_file_name.empty() )
  {
    if( deserializeSystem( serialized_file_name ) == EXIT_FAILURE )
    {
      return EXIT_FAILURE;
    }
    return executeSimLoop();
  }

  // The user must provide the path to an xml scene file
  if( argc != optind + 1 )
  {
    std::cerr << "Invalid arguments. Must provide a single xml scene file name." << std::endl;
    return EXIT_FAILURE;
  }

  // Attempt to load the user-provided scene
  if( !loadXMLScene( std::string{ argv[optind] } ) )
  {
    return EXIT_FAILURE;
  }

  // Override the default end time with the requested one, if provided
  if( end_time_override > 0.0 )
  {
    g_end_time = end_time_override;
  }

  // Compute the data output rate
  assert( g_dt.positive() );
  // If the user provided an output frequency
  if( output_frequency != 0 )
  {
    const Rational<std::intmax_t> potential_steps_per_frame{ std::intmax_t( 1 ) / ( g_dt * std::intmax_t( output_frequency ) ) };
    if( !potential_steps_per_frame.isInteger() )
    {
      std::cerr << "Timestep and output frequency do not yield an integer number of timesteps for data output. Exiting." << std::endl;
      return EXIT_FAILURE;
    }
    g_steps_per_save = potential_steps_per_frame.numerator();
  }
  // Otherwise default to dumping every frame
  else
  {
    g_steps_per_save = 1;
  }
  assert( g_end_time > 0.0 );
  g_save_number_width = MathUtilities::computeNumDigits( 1 + ceil( g_end_time / scalar( g_dt ) ) / g_steps_per_save );

  printCompileInfo( std::cout );
  assert( g_sim.state().q().size() % 3 == 0 );
  std::cout << "Body count: " << g_sim.state().q().size() / 3 << std::endl;

  // If there are any intitial collisions, warn the user
  //{
  //  std::map<std::string,unsigned> collision_counts;
  //  std::map<std::string,scalar> collision_depths;
  //  g_sim.computeNumberOfCollisions( collision_counts, collision_depths );
  //  assert( collision_counts.size() == collision_depths.size() );
  //  if( !collision_counts.empty() ) { std::cout << "Warning, initial collisions detected (name : count : total_depth):" << std::endl; }
  //  for( const std::pair<std::string,unsigned>& count_pair : collision_counts )
  //  {
  //    const std::string& constraint_name = count_pair.first;
  //    const unsigned& constraint_count = count_pair.second;
  //    assert( collision_depths.find( constraint_name ) != collision_depths.end() );
  //    const scalar& constraint_depth = collision_depths[constraint_name];
  //    std::string depth_string;
  //    if( !std::isnan( constraint_depth ) )
  //    {
  //      depth_string = StringUtilities::convertToString( constraint_depth );
  //    }
  //    else
  //    {
  //      depth_string = "depth_computation_not_supported";
  //    }
  //    std::cout << "   " << constraint_name << " : " << constraint_count << " : " << depth_string << std::endl;
  //  }
  //}

  if( g_end_time == SCALAR_INFINITY )
  {
    std::cout << "No end time specified. Simulation will run indefinitely." << std::endl;
  }

  return executeSimLoop();
}