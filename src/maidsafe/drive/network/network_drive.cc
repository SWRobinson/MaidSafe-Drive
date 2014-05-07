/*  Copyright 2011 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#ifdef USES_WINMAIN
#include <Windows.h>
#include <shellapi.h>
#endif
#include <signal.h>

#include <functional>
#include <iostream>  // NOLINT
#include <memory>
#include <string>
#include <fstream>  // NOLINT
#include <iterator>

#ifndef MAIDSAFE_WIN32
#include <locale>  // NOLINT
#else
#include "boost/locale/generator.hpp"
#endif

#include "boost/filesystem.hpp"
#include "boost/program_options.hpp"
#include "boost/preprocessor/stringize.hpp"
#include "boost/system/error_code.hpp"

#include "maidsafe/common/crypto.h"
#include "maidsafe/common/error.h"
#include "maidsafe/common/log.h"
#include "maidsafe/common/process.h"
#include "maidsafe/common/rsa.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/common/application_support_directories.h"
#include "maidsafe/common/types.h"
#include "maidsafe/common/data_stores/local_store.h"

#include "maidsafe/passport/types.h"
#include "maidsafe/passport/passport.h"

#include "maidsafe/nfs/client/maid_node_nfs.h"

#ifdef MAIDSAFE_WIN32
#include "maidsafe/drive/win_drive.h"
#else
#include "maidsafe/drive/unix_drive.h"
#endif
#include "maidsafe/drive/tools/launcher.h"

namespace fs = boost::filesystem;
namespace po = boost::program_options;

namespace maidsafe {

namespace drive {

namespace {

#ifdef MAIDSAFE_WIN32
typedef CbfsDrive<nfs_client::MaidNodeNfs> NetworkDrive;
#else
typedef FuseDrive<nfs_client::MaidNodeNfs> NetworkDrive;
#endif

fs::path g_root, g_temp, g_storage;
NetworkDrive* g_network_drive(nullptr);
std::once_flag g_unmount_flag;
const std::string kConfigFile("maidsafe_network_drive.conf");
std::string g_error_message;
int g_return_code(0);
bool g_call_once_(false);
std::vector<passport::PublicPmid> g_pmids_from_file_;
AsioService g_asio_service_(2);
std::shared_ptr<nfs_client::MaidNodeNfs> g_client_nfs_;

void CreateDir(const fs::path& dir) {
  boost::system::error_code error_code;
  if (!fs::create_directories(dir, error_code) || error_code) {
    g_error_message = std::string("Failed to create ") + dir.string() + ": " + error_code.message();
    g_return_code = error_code.value();
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::filesystem_io_error));
  }
}

void SetUpTempDirectory() {
  boost::system::error_code error_code;
  g_temp = fs::unique_path(fs::temp_directory_path() / "MaidSafe_Test_Filesystem_%%%%-%%%%-%%%%");
  CreateDir(g_temp);
  LOG(kInfo) << "Created temp directory " << g_temp;
}

void RemoveTempDirectory() {
  boost::system::error_code error_code;
  if (fs::remove_all(g_temp, error_code) == 0 || error_code)
    LOG(kWarning) << "Failed to remove g_temp " << g_temp << ": " << error_code.message();
  else
    LOG(kInfo) << "Removed " << g_temp;
}

void SetUpRootDirectory(fs::path base_dir) {
#ifdef MAIDSAFE_WIN32
  g_root = drive::GetNextAvailableDrivePath();
#else
  g_root = fs::unique_path(base_dir / "MaidSafe_Root_Filesystem_%%%%-%%%%-%%%%");
  CreateDir(g_root);
#endif
  LOG(kInfo) << "Set up g_root at " << g_root;
}

void RemoveRootDirectory() {
  boost::system::error_code error_code;
  if (fs::exists(g_root, error_code)) {
    if (fs::remove_all(g_root, error_code) == 0 || error_code) {
      LOG(kWarning) << "Failed to remove root directory " << g_root << ": "
                    << error_code.message();
    } else {
      LOG(kInfo) << "Removed " << g_root;
    }
  }
}

fs::path SetUpStorageDirectory() {
  boost::system::error_code error_code;
  fs::path storage_path(
      fs::unique_path(fs::temp_directory_path() / "MaidSafe_Test_ChunkStore_%%%%-%%%%-%%%%"));
  CreateDir(storage_path);
  g_storage = storage_path;
  LOG(kInfo) << "Created storage_path " << storage_path;
  return storage_path;
}

void RemoveStorageDirectory(const fs::path& storage_path) {
  boost::system::error_code error_code;
  if (fs::remove_all(storage_path, error_code) == 0 || error_code) {
    LOG(kWarning) << "Failed to remove storage_path " << storage_path << ": "
                  << error_code.message();
  } else {
    LOG(kInfo) << "Removed " << storage_path;
  }
}


void Unmount() {
  std::call_once(g_unmount_flag, [&] {
    g_network_drive->Unmount();
    g_network_drive = nullptr;
  });
}

#ifdef MAIDSAFE_WIN32

process::ProcessInfo GetParentProcessInfo(const Options& options) {
  return process::ProcessInfo(options.parent_handle);
}

BOOL CtrlHandler(DWORD control_type) {
  LOG(kInfo) << "Received console control signal " << control_type << ".  Unmounting.";
  if (!g_network_drive)
    return FALSE;
  Unmount();
  return TRUE;
}

void SetSignalHandler() {
  if (!SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(&CtrlHandler), TRUE)) {
    g_error_message = "Failed to set control handler.\n\n";
    g_return_code = 16;
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::unknown));
  }
}

#else

process::ProcessInfo GetParentProcessInfo(const Options& /*options*/) {
  return getppid();
}

void SetSignalHandler() {}

#endif

std::string GetStringFromProgramOption(const std::string& option_name,
                                       const po::variables_map& variables_map) {
  if (variables_map.count(option_name)) {
    std::string option_string(variables_map.at(option_name).as<std::string>());
    LOG(kInfo) << option_name << " set to " << option_string;
    return option_string;
  } else {
    return "";
  }
}

po::options_description VisibleOptions() {
  boost::system::error_code error_code;
  po::options_description options("NetworkDrive options");
  options.add_options()
#ifdef MAIDSAFE_WIN32
      ("mount_dir,D", po::value<std::string>(), " virtual drive letter (required)")
#else
      ("mount_dir,D", po::value<std::string>(), " virtual drive mount point (required)")
#endif
      ("storage_dir,S", po::value<std::string>(), " directory to store chunks (required)")
      ("unique_id,U", po::value<std::string>(), " unique identifier (required)")
      ("parent_id,R", po::value<std::string>(), " root parent directory identifier (required)")
      ("drive_name,N", po::value<std::string>()->default_value(
                            maidsafe::RandomAlphaNumericString(10)),
                       " virtual drive name")
      ("create,C", " Must be called on first run")
      ("check_data,Z", " check all data in chunkstore")
      ("peer", po::value<std::string>(), "Endpoint of peer, if using network VFS.")
      ("key_index,k", po::value<int>()->default_value(10),
                      "The index of key to be used as client")
      ("keys_path", po::value<std::string>()->default_value(fs::path(
                       fs::temp_directory_path(error_code) / "key_directory.dat").string()),
                    "Path to keys file");
  return options;
}

po::options_description HiddenOptions() {
  po::options_description options("Hidden options");
  options.add_options()
      ("help,h", "help message")
      ("shared_memory", po::value<std::string>(), "shared memory name (IPC)");
  return options;
}

template <typename Char>
po::variables_map ParseAllOptions(int argc, Char* argv[],
                                  const po::options_description& command_line_options,
                                  const po::options_description& config_file_options) {
  po::variables_map variables_map;
  try {
    // Parse command line
    po::store(po::basic_command_line_parser<Char>(argc, argv).options(command_line_options).
                  allow_unregistered().run(), variables_map);
    po::notify(variables_map);

    // Try to open local or main config files
    std::ifstream local_config_file(kConfigFile.c_str());
    fs::path main_config_path(fs::path(GetUserAppDir() / kConfigFile));
    std::ifstream main_config_file(main_config_path.string().c_str());

    // Try local first for testing
    if (local_config_file) {
      std::cout << "Using local config file \"./" << kConfigFile << "\"";
      po::store(parse_config_file(local_config_file, config_file_options), variables_map);
      po::notify(variables_map);
    } else if (main_config_file) {
      std::cout << "Using main config file \"" << main_config_path << "\"\n";
      po::store(parse_config_file(main_config_file, config_file_options), variables_map);
      po::notify(variables_map);
    }
  }
  catch (const std::exception& e) {
    g_error_message = "Fatal error:\n  " + std::string(e.what()) +
                      "\nRun with -h to see all options.\n\n";
    g_return_code = 32;
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
  return variables_map;
}

void HandleHelp(const po::variables_map& variables_map) {
  if (variables_map.count("help")) {
    std::ostringstream stream;
    stream << VisibleOptions() << "\nThese can also be set via a config file at \"./"
           << kConfigFile << "\" or at " << fs::path(GetUserAppDir() / kConfigFile) << "\n\n";
    g_error_message = stream.str();
    g_return_code = 0;
    throw MakeError(CommonErrors::success);
  }
}

bool GetFromIpc(const po::variables_map& variables_map, Options& options) {
  if (variables_map.count("shared_memory")) {
    ReadAndRemoveInitialSharedMemory(variables_map.at("shared_memory").as<std::string>(), options);
    return true;
  }
  return false;
}

void GetFromProgramOptions(const po::variables_map& variables_map, Options& options) {
  options.mount_path = GetStringFromProgramOption("mount_dir", variables_map);
  options.storage_path = GetStringFromProgramOption("storage_dir", variables_map);
  auto unique_id(GetStringFromProgramOption("unique_id", variables_map));
  if (!unique_id.empty())
    options.unique_id = Identity(unique_id);
  else
    options.unique_id = maidsafe::Identity(maidsafe::RandomString(64));
  auto parent_id(GetStringFromProgramOption("parent_id", variables_map));
  if (!parent_id.empty())
    options.root_parent_id = Identity(parent_id);
  else
    options.root_parent_id = maidsafe::Identity(maidsafe::RandomString(64));

  options.drive_name = GetStringFromProgramOption("drive_name", variables_map);
  options.create_store = (variables_map.count("create") != 0);
  options.keys_path = GetStringFromProgramOption("keys_path", variables_map);
  options.peer_endpoint = GetStringFromProgramOption("peer", variables_map);
  options.key_index = variables_map.at("key_index").as<int>();
}

void ValidateOptions(const Options& options) {
  std::string error_message;
  g_return_code = 0;
  if (options.mount_path.empty()) {
    error_message += "  mount_dir must be set\n";
    ++g_return_code;
  }
  if (options.storage_path.empty()) {
    error_message += "  chunk_store must be set\n";
    g_return_code += 2;
  }
//   if (!options.unique_id.IsInitialised()) {
//     error_message += "  unique_id must be set to a 64 character string\n";
//     g_return_code += 4;
//   }
//   if (!options.root_parent_id.IsInitialised()) {
//     error_message += "  parent_id must be set to a 64 character string\n";
//     g_return_code += 8;
//   }

  if (g_return_code) {
    g_error_message = "Fatal error:\n" + error_message + "\nRun with -h to see all options.\n\n";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
}

void MonitorParentProcess(const Options& options) {
  auto parent_process_info(GetParentProcessInfo(options));
  while (g_network_drive && process::IsRunning(parent_process_info))
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  Unmount();
}

boost::asio::ip::udp::endpoint GetBootstrapEndpoint(const std::string& peer) {
  size_t delim = peer.rfind(':');
  boost::asio::ip::udp::endpoint ep;
  ep.port(boost::lexical_cast<uint16_t>(peer.substr(delim + 1)));
  ep.address(boost::asio::ip::address::from_string(peer.substr(0, delim)));
  LOG(kInfo) << "Going to bootstrap off endpoint " << ep;
  return ep;
}

void RoutingJoin(routing::Routing& routing,
                 const std::vector<boost::asio::ip::udp::endpoint>& peer_endpoints) {
  std::shared_ptr<std::promise<bool>> join_promise(std::make_shared<std::promise<bool>>());
  routing::Functors functors_;
  functors_.network_status = [join_promise](int result) {
//     std::cout << "Network health: " << result << std::endl;
    if ((result == 100) && (!g_call_once_)) {
      g_call_once_ = true;
      join_promise->set_value(true);
    }
  };
  functors_.typed_message_and_caching.group_to_group.message_received =
      [&](const routing::GroupToGroupMessage &msg) {
        g_client_nfs_->HandleMessage(msg); };  // NOLINT
  functors_.typed_message_and_caching.group_to_single.message_received =
      [&](const routing::GroupToSingleMessage &msg) {
        g_client_nfs_->HandleMessage(msg);
      };  // NOLINT
  functors_.typed_message_and_caching.single_to_group.message_received =
      [&](const routing::SingleToGroupMessage &msg) {
        g_client_nfs_->HandleMessage(msg); };  // NOLINT
  functors_.typed_message_and_caching.single_to_single.message_received =
      [&](const routing::SingleToSingleMessage &msg) {
        g_client_nfs_->HandleMessage(msg); };  // NOLINT
  functors_.request_public_key =
      [&](const NodeId & node_id, const routing::GivePublicKeyFunctor & give_key) {
        nfs::detail::PublicPmidHelper temp_helper;
        nfs::detail::DoGetPublicKey(*g_client_nfs_, node_id, give_key,
                                    g_pmids_from_file_, temp_helper);
      };
  routing.Join(functors_, peer_endpoints);
  auto future(std::move(join_promise->get_future()));
  auto status(future.wait_for(std::chrono::seconds(10)));
  if (status == std::future_status::timeout || !future.get()) {
    std::cout << "can't join routing network" << std::endl;
    BOOST_THROW_EXCEPTION(MakeError(RoutingErrors::not_connected));
  }
  std::cout << "Client node joined routing network" << std::endl;
}

int MountAndWaitForIpcNotification(const Options& options, NetworkDrive& drive) {
  // Start a thread to poll the parent process' continued existence *before* calling drive.Mount().
  std::thread poll_parent([&] { MonitorParentProcess(options); });
  drive.Mount();
  // Drive should already be unmounted by this point, but we need to make 'g_network_drive' null to
  // allow 'poll_parent' to join.
//   Unmount();
  poll_parent.join();
  return 0;
}

int MountAndWaitForSignal(NetworkDrive& drive) {
  drive.Mount();
  Unmount();
  return 0;
}

bool CreateAccount(std::shared_ptr<passport::Maid> maid,
                   std::shared_ptr<passport::Anmaid> anmaid,
                   std::shared_ptr<passport::Pmid> pmid) {
  passport::PublicPmid::Name pmid_name(Identity(pmid->name().value));
  passport::PublicMaid public_maid(*maid);
  {
    passport::PublicAnmaid public_anmaid(*anmaid);
    auto future(g_client_nfs_->CreateAccount(nfs_vault::AccountCreation(public_maid,
                                                                        public_anmaid)));
    auto status(future.wait_for(boost::chrono::seconds(3)));
    if (status == boost::future_status::timeout) {
      std::cout << "can't create account" << std::endl;
      BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
    }
    if (future.has_exception()) {
//       std::cout << "having error during create account" << std::endl;
      try {
        future.get();
      } catch (const maidsafe_error& error) {
//         std::cout << "caught a maidsafe_error : " << error.what() << std::endl;
        if (error.code() == make_error_code(VaultErrors::account_already_exists))
          return true;
      } catch (...) {
        std::cout << "caught an unknown exception" << std::endl;
      }
    }
  }

  // waiting for syncs resolved
  boost::this_thread::sleep_for(boost::chrono::seconds(2));
  std::cout << "Account created for maid " << HexSubstr(public_maid.name()->string())
            << std::endl;
  // before register pmid, need to store pmid to network first
  g_client_nfs_->Put(passport::PublicPmid(*pmid));
  boost::this_thread::sleep_for(boost::chrono::seconds(2));

  g_client_nfs_->RegisterPmid(nfs_vault::PmidRegistration(*maid, *pmid, false));
  boost::this_thread::sleep_for(boost::chrono::seconds(3));
  auto future(g_client_nfs_->GetPmidHealth(pmid_name));
  auto status(future.wait_for(boost::chrono::seconds(3)));
  if (status == boost::future_status::timeout) {
    std::cout << "can't fetch pmid health" << std::endl;
    BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
  }
  std::cout << "The fetched PmidHealth for pmid_name " << HexSubstr(pmid_name.value.string())
            << " is " << future.get() << std::endl;
  // waiting for the GetPmidHealth updating corresponding accounts
  boost::this_thread::sleep_for(boost::chrono::seconds(3));
  LOG(kInfo) << "Pmid Registered created for the client node to store chunks";

  return false;
}

int MountAndWait(const Options& options, bool use_ipc) {
  std::vector<passport::detail::AnmaidToPmid> all_keychains =
      maidsafe::passport::detail::ReadKeyChainList(options.keys_path);
  for (auto& key_chain : all_keychains)
    g_pmids_from_file_.push_back(passport::PublicPmid(key_chain.pmid));

  std::shared_ptr<passport::Maid> maid;
  std::shared_ptr<passport::Anmaid> anmaid;
  std::shared_ptr<passport::Pmid> pmid;
  if (options.key_index != -1) {
    passport::detail::AnmaidToPmid key_chain(all_keychains[options.key_index]);
    maid.reset(new passport::Maid(key_chain.maid));
    anmaid.reset(new passport::Anmaid(key_chain.anmaid));
    pmid.reset(new passport::Pmid(key_chain.pmid));
  } else {
    crypto::AES256Key symm_key(options.symm_key);
    crypto::AES256InitialisationVector symm_iv(options.symm_iv);
    crypto::CipherText encrypted_maid(NonEmptyString(options.encrypted_maid));
    crypto::CipherText encrypted_pmid(NonEmptyString(options.encrypted_pmid));
    maid.reset(new passport::Maid(passport::DecryptMaid(encrypted_maid, symm_key, symm_iv)));
    pmid.reset(new passport::Pmid(passport::DecryptPmid(encrypted_pmid, symm_key, symm_iv)));
  }

  routing::Routing client_routing_(*maid);
  passport::PublicPmid::Name pmid_name(Identity(pmid->name().value));
  g_client_nfs_.reset(new nfs_client::MaidNodeNfs(g_asio_service_, client_routing_, pmid_name));

  std::vector<boost::asio::ip::udp::endpoint> peer_endpoints;
  if (!options.peer_endpoint.empty())
    peer_endpoints.push_back(GetBootstrapEndpoint(options.peer_endpoint));
  RoutingJoin(client_routing_, peer_endpoints);

  bool account_exists(true);
  if (options.key_index != -1)
    account_exists = CreateAccount(maid, anmaid, pmid);

  maidsafe::Identity unique_id(options.unique_id);
  maidsafe::Identity root_parent_id(options.root_parent_id);
  if ((options.key_index != -1) ||
      (!unique_id.IsInitialised() || !root_parent_id.IsInitialised())) {
    passport::PublicMaid public_maid(*maid);
    unique_id = maidsafe::Identity(crypto::Hash<crypto::SHA512>(public_maid.name()->string()));
    root_parent_id = maidsafe::Identity(crypto::Hash<crypto::SHA512>(unique_id.string()));
  }

  std::cout << "unique_id : " << HexSubstr(unique_id.string()) << std::endl;
  std::cout << "root_parent_id : " << HexSubstr(root_parent_id.string()) << std::endl;

  bool create_store(!account_exists);
  NetworkDrive drive(g_client_nfs_, unique_id, root_parent_id, options.mount_path, GetUserAppDir(),
                     options.drive_name, options.mount_status_shared_object_name, create_store);
  g_network_drive = &drive;
#ifdef MAIDSAFE_WIN32
  g_network_drive->SetGuid(BOOST_PP_STRINGIZE(PRODUCT_ID));
#endif
  if (use_ipc) {
    return MountAndWaitForIpcNotification(options, drive);
  } else {
    return MountAndWaitForSignal(drive);
  }
}

}  // unnamed namespace

}  // namespace drive

}  // namespace maidsafe

#ifdef USES_WINMAIN
int CALLBACK wWinMain(HINSTANCE /*handle_to_instance*/, HINSTANCE /*handle_to_previous_instance*/,
                      PWSTR /*command_line_args_without_program_name*/, int /*command_show*/) {
  int argc(0);
  LPWSTR* argv(nullptr);
  argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#else
int main(int argc, char* argv[]) {
#endif
#ifdef MAIDSAFE_WIN32
  std::locale::global(boost::locale::generator().generate(""));
#else
  std::locale::global(std::locale(""));
#endif
  maidsafe::log::Logging::Instance().Initialise(argc, argv);
  fs::path::imbue(std::locale());
  boost::system::error_code error_code;
  try {
    // Set up command line options and config file options
    auto visible_options(maidsafe::drive::VisibleOptions());
    po::options_description command_line_options, config_file_options;
    command_line_options.add(visible_options).add(maidsafe::drive::HiddenOptions());
    config_file_options.add(visible_options);

    // Read in options
    auto variables_map(maidsafe::drive::ParseAllOptions(argc, argv, command_line_options,
                                                        config_file_options));
    maidsafe::drive::HandleHelp(variables_map);
    maidsafe::drive::Options options;
    bool using_ipc(maidsafe::drive::GetFromIpc(variables_map, options));
    if (!using_ipc) {
      maidsafe::drive::SetUpTempDirectory();
      maidsafe::drive::GetFromProgramOptions(variables_map, options);
      maidsafe::drive::SetUpRootDirectory(maidsafe::GetHomeDir());
      options.mount_path = maidsafe::drive::g_root;
      options.storage_path = maidsafe::drive::SetUpStorageDirectory();
    }

    // Validate options and run the Drive
    maidsafe::drive::ValidateOptions(options);
    if (using_ipc) {
      return maidsafe::drive::MountAndWait(options, true);
    } else {
      maidsafe::drive::SetSignalHandler();
      maidsafe::drive::MountAndWait(options, false);

      maidsafe::drive::RemoveTempDirectory();
      maidsafe::drive::RemoveStorageDirectory(options.storage_path);
      maidsafe::drive::RemoveRootDirectory();
    }
  }
  catch (const std::exception& e) {
    if (!maidsafe::drive::g_error_message.empty()) {
      std::cout << maidsafe::drive::g_error_message;
      return maidsafe::drive::g_return_code;
    }
    LOG(kError) << "Exception: " << e.what();
  }
  catch (...) {
    LOG(kError) << "Exception of unknown type!";
  }
  return 64;
}
