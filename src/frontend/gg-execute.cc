/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <iostream>
#include <string>
#include <memory>
#include <sys/fcntl.h>
#include <getopt.h>
#include <vector>
#include <unordered_set>

#include "execution/response.hh"
#include "net/requests.hh"
#include "storage/backend.hh"
#include "thunk/ggutils.hh"
#include "thunk/factory.hh"
#include "thunk/thunk_reader.hh"
#include "thunk/thunk_writer.hh"
#include "thunk/thunk.hh"
#include "util/child_process.hh"
#include "util/digest.hh"
#include "util/exception.hh"
#include "util/path.hh"
#include "util/temp_dir.hh"
#include "util/temp_file.hh"
#include "util/util.hh"

using namespace std;
using namespace gg;
using namespace gg::thunk;
using ReductionResult = gg::cache::ReductionResult;

const bool sandboxed = ( getenv( "GG_SANDBOXED" ) != NULL );
const string temp_dir_template = "/tmp/thunk-execute";
const string temp_file_template = "/tmp/thunk-file";

vector<string> execute_thunk( const Thunk & original_thunk )
{
  Thunk thunk = original_thunk;

  // if ( not thunk.can_be_executed() ) {
  //   /* Let's see if we can redudce this thunk to an order one thunk by updating
  //   the infiles */
  //   for ( const Thunk::DataItem & dep_item : thunk.thunks() ) {
  //     /* let's check if we have a reduction of this infile */
  //     vector<ThunkOutput> new_outputs;
  //
  //     auto result = gg::cache::check( dep_item.first );
  //
  //     if ( not result.initialized() or
  //          gg::hash::type( result->hash ) == gg::ObjectType::Thunk ) {
  //       throw runtime_error( "thunk is not executable and cannot be "
  //                            "reduced to an executable thunk" );
  //     }
  //
  //     thunk.update_data( dep_item.first, result->hash );
  //   }
  //
  //   thunk.set_hash( ThunkWriter::write( thunk ) );
  //
  //   cerr << "thunk:" << original_thunk.hash() << " reduced to "
  //        << "thunk:" << thunk.hash() << "." << endl;
  // }

  /* when executing the thunk, we create a temp directory, and execute the thunk
     in that directory. then we take the outfile, compute the hash, and move it
     to the .gg directory. */

  // PREPARING THE ENV
  TempDirectory exec_dir { temp_dir_template };
  roost::path exec_dir_path { exec_dir.name() };
  roost::path outfile_path { "output" };

  // EXECUTING THE THUNK
  if ( not sandboxed ) {
    ChildProcess process {
      thunk.hash(),
      [thunk, &exec_dir_path]() {
        roost::create_directories( exec_dir_path );
        CheckSystemCall( "chdir", chdir( exec_dir_path.string().c_str() ) );
        return thunk.execute();
      }
    };

    while ( not process.terminated() ) {
      process.wait();
    }

    if ( process.exit_status() ) {
      try {
        process.throw_exception();
      }
      catch ( const exception & ex ) {
        throw_with_nested( ExecutionError {} );
      }
    }
  }
  else {
    auto allowed_files = thunk.get_allowed_files();

    SandboxedProcess process {
      "execute(" + thunk.hash().substr( 0, 5 ) + ")",
      allowed_files,
      [thunk]() {
        return thunk.execute();
      },
      [&exec_dir_path] () {
        roost::create_directories( exec_dir_path );
        CheckSystemCall( "chdir", chdir( exec_dir_path.string().c_str() ) );
      }
    };

    try {
      process.execute();
    }
    catch( const exception & ex ) {
      throw_with_nested( ExecutionError {} );
    }
  }

  vector<string> output_hashes;

  // GRABBING THE OUTPUTS & CREATING CACHE ENTRIES
  for ( const string & output : thunk.outputs() ) {
    roost::path outfile { exec_dir_path / output };

    if ( not roost::exists( outfile ) ) {
      throw ExecutionError {};
    }

    /* let's check if the output is a thunk or not */
    string outfile_hash = gg::hash::file( outfile );
    roost::path outfile_gg = gg::paths::blob_path( outfile_hash );

    if ( not roost::exists( outfile_gg ) ) {
      roost::move_file( outfile, outfile_gg );
    } else {
      roost::remove( outfile );
    }

    output_hashes.emplace_back( move( outfile_hash ) );
  }

  for ( size_t i = thunk.outputs().size() - 1; i < thunk.outputs().size(); i-- ) {
    const string & output = thunk.outputs().at( i );
    const string & outfile_hash = output_hashes.at( i );

    gg::cache::insert( original_thunk.output_hash( output ), outfile_hash );
    if ( original_thunk.hash() != thunk.hash() ) {
      gg::cache::insert( thunk.output_hash( output ), outfile_hash );
    }

    if ( i == 0 ) {
      gg::cache::insert( original_thunk.hash(), outfile_hash );
      if ( original_thunk.hash() != thunk.hash() ) {
        gg::cache::insert( thunk.hash(), outfile_hash );
      }
    }
  }

  return output_hashes;
}

void do_cleanup( const Thunk & thunk )
{
  unordered_set<string> infile_hashes;

  infile_hashes.emplace( thunk.hash() );

  for ( const Thunk::DataItem & item : thunk.values() ) {
    infile_hashes.emplace( item.first );
  }

  for ( const Thunk::DataItem & item : thunk.executables() ) {
    infile_hashes.emplace( item.first );
  }

  for ( const string & blob : roost::list_directory( gg::paths::blobs() ) ) {
    const roost::path path = gg::paths::blob_path( blob );
    if ( ( not roost::is_directory( path ) ) and infile_hashes.count( blob ) == 0 ) {
      roost::remove( path );
    }
  }
}

void fetch_dependencies( unique_ptr<StorageBackend> & storage_backend,
                         const Thunk & thunk )
{
  try {
    vector<storage::GetRequest> download_items;

    auto check_dep =
      [&download_items]( const Thunk::DataItem & item ) -> void
      {
        const auto target_path = gg::paths::blob_path( item.first );

        if ( not roost::exists( target_path )
             or roost::file_size( target_path ) != gg::hash::size( item.first ) ) {
          download_items.push_back( { item.first, target_path } );
        }
      };

    for_each( thunk.values().cbegin(), thunk.values().cend(),
              check_dep );

    for_each( thunk.executables().cbegin(), thunk.executables().cend(),
              check_dep );

    if ( download_items.size() > 0 ) {
      storage_backend->get( download_items );
    }
  }
  catch ( const exception & ex ) {
    throw_with_nested( FetchDependenciesError {} );
  }
}

void upload_output( unique_ptr<StorageBackend> & storage_backend,
                    const vector<string> & output_hashes )
{
  try {
    vector<storage::PutRequest> requests;
    for ( const string & output_hash : output_hashes ) {
      requests.push_back( { gg::paths::blob_path( output_hash ), output_hash,
                            gg::hash::to_hex( output_hash ) } );
    }
    storage_backend->put( requests );
  }
  catch ( const exception & ex ) {
    throw_with_nested( UploadOutputError {} );
  }
}

void usage( const char * argv0 )
{
  cerr << "Usage: " << argv0 << "[options] THUNK-HASH..." << endl
  << endl
  << "Options: " << endl
  << " -g, --get-dependencies  Fetch the missing dependencies from the remote storage" << endl
  << " -p, --put-output        Upload the output to the remote storage" << endl
  << " -F, --fix-permissions   Make sure that all the executables have correct permissions" << endl
  << " -C, --cleanup           Remove unnecessary blobs in .gg dir" << endl
  << endl;
}

int main( int argc, char * argv[] )
{
  try {
    if ( argc <= 0 ) {
      abort();
    }

    if ( argc < 2 ) {
      usage( argv[ 0 ] );
      return to_underlying( JobStatus::OperationalFailure );
    }

    bool get_dependencies = false;
    bool put_output = false;
    bool cleanup = false;
    bool fix_permissions = false;
    unique_ptr<StorageBackend> storage_backend;

    const option command_line_options[] = {
      { "get-dependencies", no_argument, nullptr, 'g' },
      { "put-output",       no_argument, nullptr, 'p' },
      { "cleanup",          no_argument, nullptr, 'C' },
      { "fix-permissions",  no_argument, nullptr, 'F' },
      { nullptr, 0, nullptr, 0 },
    };

    while ( true ) {
      const int opt = getopt_long( argc, argv, "gpCF", command_line_options, nullptr );

      if ( opt == -1 ) {
        break;
      }

      switch ( opt ) {
      case 'g': get_dependencies = fix_permissions = true; break;
      case 'p': put_output = true; break;
      case 'C': cleanup = true; break;
      case 'F': fix_permissions = true; break;

      default:
        throw runtime_error( "invalid option: " + string { argv[ optind - 1 ] } );
      }
    }

    vector<string> thunk_hashes;

    for ( int i = optind; i < argc; i++ ) {
      thunk_hashes.push_back( argv[ i ] );
    }

    if ( thunk_hashes.size() == 0 ) {
      usage( argv[ 0 ] );
      return to_underlying( JobStatus::OperationalFailure );
    }

    gg::models::init();

    for ( const string & thunk_hash : thunk_hashes ) {
      /* take out an advisory lock on the thunk, in case
         other gg-execute processes are running at the same time */
      const string thunk_path = gg::paths::blob_path( thunk_hash ).string();
      FileDescriptor raw_thunk { CheckSystemCall( "open( " + thunk_path + " )",
                                                  open( thunk_path.c_str(), O_RDONLY ) ) };
      raw_thunk.block_for_exclusive_lock();

      Thunk thunk = ThunkReader::read( thunk_path );

      if ( get_dependencies or put_output ) {
        storage_backend = StorageBackend::create_backend( gg::remote::storage_backend_uri() );
      }

      if ( cleanup ) {
        do_cleanup( thunk );
      }

      if ( get_dependencies ) {
        fetch_dependencies( storage_backend, thunk );
      }

      if ( fix_permissions ) {
        for ( const Thunk::DataItem & item : thunk.executables() ) {
          roost::make_executable( gg::paths::blob_path( item.first ) );
        }
      }

      vector<string> output_hashes = execute_thunk( thunk );

      if ( put_output ) {
        upload_output( storage_backend, output_hashes );
      }
    }

    return to_underlying( JobStatus::Success );
  }
  catch ( const FetchDependenciesError & e ) {
    print_nested_exception( e );
    return to_underlying( JobStatus::FetchDependenciesFailure );
  }
  catch ( const ExecutionError & e ) {
    print_nested_exception( e );
    return to_underlying( JobStatus::ExecutionFailure );
  }
  catch ( const UploadOutputError & e ) {
    print_nested_exception( e );
    return to_underlying( JobStatus::UploadOutputFailure );
  }
  catch ( const exception & e ) {
    print_exception( argv[ 0 ], e );
    return to_underlying( JobStatus::OperationalFailure );
  }
}
