// HDF5File.cpp
//
// Breannan Smith
// Last updated: 09/03/2015

#include "HDF5File.h"

#include <cassert>

#include "Utilities.h"

#ifdef USE_HDF5
using HDFGID = HDFID<H5Gclose>;
using HDFTID = HDFID<H5Tclose>;
using HDFSID = HDFID<H5Sclose>;
using HDFDID = HDFID<H5Dclose>;
#endif

HDF5File::HDF5File()
: m_hdf_file_id( 0 )
, m_file_opened( false )
{}

HDF5File::HDF5File( const std::string& file_name, const AccessType& access_type )
: m_hdf_file_id( 0 )
, m_file_opened( false )
{
  open( file_name, access_type );
}

HDF5File::~HDF5File()
{
  #ifdef USE_HDF5
  if( m_file_opened )
  {
    assert( m_hdf_file_id >= 0 );
    H5Fclose( m_hdf_file_id );
  }
  #endif
}

int HDF5File::fileID()
{
  return m_hdf_file_id;
}

void HDF5File::open( const std::string& file_name, const AccessType& access_type )
{
  #ifdef USE_HDF5
  // Attempt to open a file
  if( access_type == READ_WRITE )
  {
    m_hdf_file_id = H5Fcreate( file_name.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT );
  }
  else
  {
    m_hdf_file_id = H5Fopen( file_name.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT );
  }
  // Check that the file successfully opened
  if( m_hdf_file_id < 0 )
  {
    throw std::string{ "Failed to open file: " } + file_name;
  }
  m_file_opened = true;
  #else
  throw std::string{ "HDF5File::HDF5File not compiled with HDF5 support" };
  #endif
}

bool HDF5File::is_open() const
{
  return m_file_opened;
}

void HDF5File::writeString( const std::string& group, const std::string& variable_name, const std::string& string_variable ) const
{
  #ifdef USE_HDF5
  // HDF5 expects an array of strings
  const char* string_data[1] = { string_variable.c_str() };
  const HDFTID file_type{ H5Tcopy( H5T_FORTRAN_S1 ) };
  if( file_type < 0 )
  {
    throw std::string{ "Failed to create HDF5 string file type" };
  }
  const herr_t set_file_size_status{ H5Tset_size( file_type, H5T_VARIABLE ) };
  if( set_file_size_status < 0 )
  {
    throw std::string{ "Failed to set HDF5 string file size" };
  }
  const HDFTID mem_type{ H5Tcopy( H5T_C_S1 ) };
  if( mem_type < 0 ) { throw std::string{ "Failed to create HDF5 string memory type" }; }
  const herr_t set_memory_size_status{ H5Tset_size( mem_type, H5T_VARIABLE ) };
  if( set_memory_size_status < 0 )
  {
    throw std::string{ "Failed to set HDF5 string memory size" };
  }
  const hsize_t dims[1] = { 1 };
  const HDFSID space{ H5Screate_simple( 1, dims, nullptr ) };
  if( space < 0 )
  {
    throw std::string{ "Failed to create HDF space" };
  }
  assert( m_hdf_file_id >= 0 );
  const HDFDID dset{ H5Dcreate2( m_hdf_file_id, ( group + "/" + variable_name ).c_str(), file_type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT ) };
  if( dset < 0 )
  {
    throw std::string{ "Failed to create HDF dataset" };
  }
  const herr_t write_status{ H5Dwrite( dset, mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, string_data ) };
  if( write_status < 0 )
  {
    throw std::string{ "Failed to write HDF data" };
  }
  #else
  throw std::string{ "HDF5File::writeString not compiled with HDF5 support" };
  #endif
}

void HDF5File::readString( const std::string& group, const std::string& variable_name, std::string& string_variable ) const
{
  #ifdef USE_HDF5
  const HDFDID dset{ H5Dopen2( m_hdf_file_id, ( group + "/" + variable_name ).c_str(), H5P_DEFAULT ) };
  if( dset < 0 )
  {
    throw std::string{ "Failed to open HDF dataset" };
  }
  const HDFTID file_type{ H5Dget_type( dset ) };
  if( file_type < 0 )
  {
    throw std::string{ "Failed to get HDF file type" };
  }
  const HDFSID space{ H5Dget_space( dset ) };
  if( space < 0 )
  {
    throw std::string{ "Failed to get HDF space" };
  }
  hsize_t dims[1];
  const int ndims{ H5Sget_simple_extent_dims( space, dims, nullptr ) };
  Utilities::ignoreUnusedVariable( ndims );
  assert( ndims == 1 ); assert( dims[0] == 1 );
  const HDFTID mem_type{ H5Tcopy( H5T_C_S1 ) };
  if( mem_type < 0 )
  {
    throw std::string{ "Failed to get HDF mem type" };
  }
  const herr_t set_size_status{ H5Tset_size( mem_type, H5T_VARIABLE ) };
  if( set_size_status < 0 )
  {
    throw std::string{ "Failed to set HDF mem type size" };
  }
  char* rdata[1] = { nullptr };
  const herr_t read_status{ H5Dread( dset, mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, rdata ) };
  if( read_status < 0 )
  {
    if( rdata[0] != nullptr ) { free( rdata[0] ); }
    throw std::string{ "Failed to read HDF data" };
  }
  string_variable = rdata[0];
  free( rdata[0] );
  #else
  throw std::string{ "HDF5File::readString not compiled with HDF5 support" };
  #endif
}

void HDF5File::createGroup( const std::string& group ) const
{
  #ifdef USE_HDF5
  const HDFGID group_id{ H5Gcreate2( m_hdf_file_id, group.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT ) };
  if( group_id < 0 )
  {
    throw std::string{ "Failed to create group: " } + group;
  }
  #else
  throw std::string{ "HDF5File::createGroup not compiled with HDF5 support" };
  #endif
}