/*
 * Sarus
 *
 * Copyright (c) 2018-2023, ETH Zurich. All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <memory>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/fstream.hpp>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/signal.h>
#include <boost/regex.hpp>

#include "common/Config.hpp"
#include "common/PathRAII.hpp"
#include "common/Utility.hpp"
#include "hooks/common/Utility.hpp"
#include "runtime/mount_utilities.hpp"
#include "hooks/ssh/SshHook.hpp"
#include "test_utility/Misc.hpp"
#include "test_utility/config.hpp"
#include "test_utility/filesystem.hpp"
#include "test_utility/OCIHooks.hpp"
#include "test_utility/unittest_main_function.hpp"

namespace rj = rapidjson;

namespace sarus {
namespace hooks {
namespace ssh {
namespace test {

class Helper {
public:
    Helper() {
        configRAII.config->userIdentity.uid = std::get<0>(idsOfUser);
        configRAII.config->userIdentity.gid = std::get<1>(idsOfUser);
    }

    ~Helper() {
        // get root privileges in case that the test failure
        // occurred while we had non-root privileges
        setRootIds();

        // undo mounts in rootfs
        for(const auto& folder : rootfsFolders) {
            umount2((rootfsDir / folder).c_str(), MNT_FORCE | MNT_DETACH);
        }

        // undo overlayfs mount in ~/.ssh
        umount2((expectedHomeDirInContainer / ".ssh").c_str(), MNT_FORCE | MNT_DETACH);

        // undo tmpfs mount on bundle directory
        umount2((bundleDir).c_str(), MNT_FORCE | MNT_DETACH);

        // kill SSH daemon
        auto pid = getSshDaemonPid();
        if(pid) {
            kill(*pid, SIGTERM);
        }

        // NOTE: the test directories are automatically removed by the PathRAII objects
    }

    void setupTestEnvironment() {
        // create tmpfs filesystem to allow overlay mounts for rootfs (performed below)
        // to succeed also when testing inside a Docker container
        sarus::common::createFoldersIfNecessary(bundleDir);
        if(mount(NULL, bundleDir.c_str(), "tmpfs", MS_NOSUID|MS_NODEV, NULL) != 0) {
            auto message = boost::format("Failed to setup tmpfs filesystem on %s: %s")
                % bundleDir
                % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }

        sarus::common::createFoldersIfNecessary(homeDirInHost,
                                                std::get<0>(idsOfUser),
                                                std::get<1>(idsOfUser));

        sarus::common::createFoldersIfNecessary(expectedHomeDirInContainer,
                                                std::get<0>(idsOfUser),
                                                std::get<1>(idsOfUser));

        // host's dropbear installation
        sarus::common::createFoldersIfNecessary(dropbearDirInHost.getPath() / "bin");
        boost::format setupDropbearCommand = boost::format{
            "cp %1% %2%/bin/dropbearmulti"
            " && ln -s %2%/bin/dropbearmulti %2%/bin/dbclient"
            " && ln -s %2%/bin/dropbearmulti %2%/bin/dropbear"
            " && ln -s %2%/bin/dropbearmulti %2%/bin/dropbearkey"
        } % sarus::common::Config::BuildTime{}.dropbearmultiBuildArtifact.string()
          % dropbearDirInHost.getPath().string();
        sarus::common::executeCommand(setupDropbearCommand.str());

        // hook's environment variables
        sarus::common::setEnvironmentVariable("HOOK_BASE_DIR", sshKeysBaseDir.string());
        sarus::common::setEnvironmentVariable("PASSWD_FILE", passwdFile.string());
        sarus::common::setEnvironmentVariable("DROPBEAR_DIR", dropbearDirInHost.getPath().string());
        sarus::common::setEnvironmentVariable("SERVER_PORT", std::to_string(serverPort));

        createConfigJSON();

        // rootfs
        for(const auto& folder : rootfsFolders) {
            auto lowerDir = "/" / folder;
            auto upperDir = bundleDir / "upper-dirs" / folder;
            auto workDir = bundleDir / "work-dirs" / folder;
            auto mergedDir = rootfsDir / folder;

            sarus::common::createFoldersIfNecessary(upperDir);
            sarus::common::createFoldersIfNecessary(workDir);
            sarus::common::createFoldersIfNecessary(mergedDir);

            runtime::mountOverlayfs(lowerDir, upperDir, workDir, mergedDir);
        }

        // set requested home dir in /etc/passwd
        auto passwd = sarus::common::PasswdDB{rootfsDir / "etc/passwd"};
        for (auto& entry : passwd.getEntries()) {
            if(entry.uid == std::get<0>(idsOfUser)) {
                entry.userHomeDirectory = "/" / boost::filesystem::relative(homeDirInContainerPasswd, rootfsDir);
            }
        }
        passwd.write(rootfsDir / "etc/passwd");
    }

    void createConfigJSON() {
        namespace rj = rapidjson;
        auto doc = test_utility::ocihooks::createBaseConfigJSON(rootfsDir, idsOfUser);
        auto& allocator = doc.GetAllocator();
        for (const auto& var : environmentVariablesInContainer) {
            doc["process"]["env"].PushBack(rj::Value{var.c_str(), allocator}, allocator);
        }
        rapidjson::Value jUserSshKey(rapidjson::kObjectType);
        jUserSshKey.AddMember(
            "com.hooks.ssh.authorize_ssh_key", 
            rj::Value{(sshKeysDirInHost / "user_key.pub").string().c_str(), doc.GetAllocator()},
            doc.GetAllocator()
        );
        if(!doc.HasMember("annotations")) {
            doc.AddMember("annotations", jUserSshKey, doc.GetAllocator());
        } else {
            doc["annotations"] = jUserSshKey;
        }

        sarus::common::writeJSON(doc, bundleDir / "config.json");
    }

    void writeContainerStateToStdin() const {
        test_utility::ocihooks::writeContainerStateToStdin(bundleDir);
    }

    void setUserIds() const {
        if(setresuid(std::get<0>(idsOfUser), std::get<0>(idsOfUser), std::get<0>(idsOfRoot)) != 0) {
            auto message = boost::format("Failed to set uid %d: %s") % std::get<0>(idsOfUser) % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }
    }

    void setRootIds() const {
        if(setresuid(std::get<0>(idsOfRoot), std::get<0>(idsOfRoot), std::get<0>(idsOfRoot)) != 0) {
            auto message = boost::format("Failed to set uid %d: %s") % std::get<0>(idsOfRoot) % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }
    }

    void setExpectedHomeDirInContainer(const boost::filesystem::path& path) {
        expectedHomeDirInContainer = rootfsDir / path;
    }

    void setHomeDirInContainerPasswd(const boost::filesystem::path& path) {
        homeDirInContainerPasswd = rootfsDir / path;
    }

    void setEnvironmentVariableInContainer(const std::string& variable) {
        environmentVariablesInContainer.push_back(variable);
    }

    void generateUserSshKeyFile() {
        std::ofstream userSshKeyFile{(sshKeysDirInHost / "user_key.pub").string()};
        userSshKeyFile << userSshKey;
    }

    void checkHostHasSshKeys() const {
        CHECK(boost::filesystem::exists(sshKeysDirInHost / "dropbear_ecdsa_host_key"));
        CHECK(boost::filesystem::exists(sshKeysDirInHost / "id_dropbear"));
        CHECK(boost::filesystem::exists(sshKeysDirInHost / "authorized_keys"));
    }

    void checkContainerHasServerKeys() const {
        CHECK(boost::filesystem::exists(expectedHomeDirInContainer / ".ssh/dropbear_ecdsa_host_key"));
        CHECK(sarus::common::getOwner(expectedHomeDirInContainer / ".ssh/dropbear_ecdsa_host_key") == idsOfUser);
    }

    void checkContainerHasClientKeys() const {
        CHECK(boost::filesystem::exists(expectedHomeDirInContainer / ".ssh/id_dropbear"));
        CHECK(sarus::common::getOwner(expectedHomeDirInContainer / ".ssh/id_dropbear") == idsOfUser);
        CHECK(boost::filesystem::exists(expectedHomeDirInContainer / ".ssh/authorized_keys"));
        CHECK(sarus::common::getOwner(expectedHomeDirInContainer / ".ssh/authorized_keys") == idsOfUser);
    }

    boost::optional<pid_t> getSshDaemonPid() const {
        auto out = sarus::common::executeCommand("ps ax -o pid,args");
        std::stringstream ss{out};
        std::string line;

        boost::smatch matches;
        boost::regex pattern("^ *([0-9]+) +/opt/oci-hooks/dropbear/bin/dropbear.*$");

        while(std::getline(ss, line)) {
            if(boost::regex_match(line, matches, pattern)) {
                return std::stoi(matches[1]);
            }
        }
        return {};
    }

    void checkContainerHasSshBinary() const {
        auto targetFile = boost::filesystem::path(rootfsDir / "usr/bin/ssh");
        CHECK(boost::filesystem::exists(targetFile));

        auto expectedScript = boost::format{
            "#!/bin/sh\n"
            "/opt/oci-hooks/dropbear/bin/dbclient -y -p %s $*\n"
        } % serverPort;
        auto actualScript = sarus::common::readFile(targetFile);
        CHECK_EQUAL(actualScript, expectedScript.str());

        auto expectedPermissions =
            boost::filesystem::owner_all |
            boost::filesystem::group_read | boost::filesystem::group_exe |
            boost::filesystem::others_read | boost::filesystem::others_exe;
        auto status = boost::filesystem::status(targetFile);
        CHECK(status.permissions() == expectedPermissions);
    }

    void checkContainerHasEnvironmentFile() const {
        auto targetFile = boost::filesystem::path(dropbearDirInContainer / "environment");
        CHECK(boost::filesystem::exists(targetFile));

        auto expectedMap = std::unordered_map<std::string, std::string>{};
        for (const auto& variable : environmentVariablesInContainer) {
            std::string key, value;
            std::tie(key, value) = sarus::common::parseEnvironmentVariable(variable);
            expectedMap[key] = value;
        }

        boost::filesystem::ifstream actualLines{targetFile};
        std::string actualLine;
        std::getline(actualLines, actualLine);
        CHECK_EQUAL(actualLine, std::string{"#!/bin/sh"});

        auto actualMap = std::unordered_map<std::string, std::string>{};
        // first line is the shebang, last line is empty, so to parse the variable definitions
        // we iterate from begin+1 to end-1
        while (std::getline(actualLines, actualLine)) {
            auto tokens = std::vector<std::string>{};
            boost::split(tokens, actualLine, boost::is_any_of(" "));
            CHECK_EQUAL(tokens[0], std::string{"export"});
            auto keyValue = std::vector<std::string>{};
            boost::split(keyValue, tokens[1], boost::is_any_of("="));
            std::string key = keyValue[0];
            std::string value = keyValue[1].substr(1, keyValue[1].size()-2); // remove first and last double-quotes
            actualMap[key] = value;
        }
        CHECK(actualMap == expectedMap);

        auto expectedPermissions =
            boost::filesystem::owner_all |
            boost::filesystem::group_read |
            boost::filesystem::others_read;
        auto status = boost::filesystem::status(targetFile);
        CHECK(status.permissions() == expectedPermissions);
    }

    void checkContainerHasEtcProfileModule() const {
        auto targetFile = boost::filesystem::path(rootfsDir / "etc/profile.d/ssh-hook.sh");
        CHECK(boost::filesystem::exists(targetFile));

        auto expectedScript = std::string(
                "#!/bin/sh\n"
                "if [ \"$SSH_CONNECTION\" ]; then\n"
                "    . /opt/oci-hooks/dropbear/environment\n"
                "fi\n");
        auto actualScript = sarus::common::readFile(targetFile);
        CHECK_EQUAL(actualScript, expectedScript);

        auto expectedPermissions =
                boost::filesystem::owner_read | boost::filesystem::owner_write |
                boost::filesystem::group_read |
                boost::filesystem::others_read;
        auto status = boost::filesystem::status(targetFile);
        CHECK(status.permissions() == expectedPermissions);
    }

    bool isUserSshKeyAuthorized() {
        std::ifstream authorizedKeysFile{(expectedHomeDirInContainer / ".ssh/authorized_keys").string()};
        if(!authorizedKeysFile.is_open()) {
            return false;
        }
        std::string line;
        while(getline(authorizedKeysFile, line)) {
            if (line.find(userSshKey, 0) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
private:
    std::tuple<uid_t, gid_t> idsOfRoot{0, 0};
    std::tuple<uid_t, gid_t> idsOfUser = test_utility::misc::getNonRootUserIds();

    test_utility::config::ConfigRAII configRAII = test_utility::config::makeConfig();
    boost::filesystem::path prefixDir = configRAII.config->json["prefixDir"].GetString();
    boost::filesystem::path passwdFile = prefixDir / "etc/passwd";
    boost::filesystem::path bundleDir = configRAII.config->json["OCIBundleDir"].GetString();
    boost::filesystem::path rootfsDir = bundleDir / configRAII.config->json["rootfsFolder"].GetString();
    boost::filesystem::path sshKeysBaseDir = configRAII.config->json["localRepositoryBaseDir"].GetString();
    std::string username = sarus::common::PasswdDB{passwdFile}.getUsername(std::get<0>(idsOfUser));
    boost::filesystem::path homeDirInHost = sshKeysBaseDir / username;
    boost::filesystem::path expectedHomeDirInContainer = rootfsDir / "home" / username;
    boost::filesystem::path homeDirInContainerPasswd = expectedHomeDirInContainer;
    boost::filesystem::path sshKeysDirInHost = homeDirInHost / ".oci-hooks/ssh/keys";
    sarus::common::PathRAII dropbearDirInHost = sarus::common::PathRAII{boost::filesystem::absolute(
                                               sarus::common::makeUniquePathWithRandomSuffix("./hook-test-dropbeardir-in-host"))};
    boost::filesystem::path dropbearDirInContainer = rootfsDir / "opt/oci-hooks/dropbear";
    std::uint16_t serverPort = 11111;
    std::vector<boost::filesystem::path> rootfsFolders = {"etc", "dev", "bin", "sbin", "usr", "lib", "lib64"}; // necessary to chroot into rootfs
    std::vector<std::string> environmentVariablesInContainer;
    std::string userSshKey{"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAvAIP2SI2ON23c6ZP1c7gQf17P25npZLgHSxfwqRKNWh27p user@test"};
};

TEST_GROUP(SSHHookTestGroup) {
};

TEST(SSHHookTestGroup, testSshHook) {
    Helper helper{};

    helper.setRootIds();
    helper.setupTestEnvironment();

    // generate + check SSH keys in local repository
    helper.setUserIds(); // keygen is executed with user privileges
    SshHook{}.generateSshKeys(true);
    SshHook{}.checkUserHasSshKeys();
    helper.setRootIds();
    helper.checkHostHasSshKeys();

    // start sshd
    helper.writeContainerStateToStdin();
    SshHook{}.startSshDaemon();
    helper.checkContainerHasClientKeys();
    helper.checkContainerHasServerKeys();
    CHECK(static_cast<bool>(helper.getSshDaemonPid()));
    helper.checkContainerHasSshBinary();
}

TEST(SSHHookTestGroup, testNonStandardHomeDir) {
    Helper helper{};

    helper.setRootIds();
    helper.setHomeDirInContainerPasswd("/users/test-home-dir");
    helper.setExpectedHomeDirInContainer("/users/test-home-dir");
    helper.setupTestEnvironment();

    // generate + check SSH keys in local repository
    helper.setUserIds(); // keygen is executed with user privileges
    SshHook{}.generateSshKeys(true);
    SshHook{}.checkUserHasSshKeys();
    helper.setRootIds();
    helper.checkHostHasSshKeys();

    // start sshd
    helper.writeContainerStateToStdin();
    SshHook{}.startSshDaemon();
    helper.checkContainerHasClientKeys();
    helper.checkContainerHasServerKeys();
    CHECK(static_cast<bool>(helper.getSshDaemonPid()));
    helper.checkContainerHasSshBinary();
}

TEST(SSHHookTestGroup, testSetEnvironmentOnLogin) {
    Helper helper{};

    helper.setRootIds();
    helper.setHomeDirInContainerPasswd("/users/test-home-dir");
    helper.setExpectedHomeDirInContainer("/users/test-home-dir");
    helper.setEnvironmentVariableInContainer("PATH=/bin:/usr/bin:/usr/local/bin:/sbin");
    helper.setEnvironmentVariableInContainer("TEST1=VariableTest1");
    helper.setEnvironmentVariableInContainer("TEST2=VariableTest2");
    helper.setupTestEnvironment();

    // generate + check SSH keys in local repository
    helper.setUserIds(); // keygen is executed with user privileges
    SshHook{}.generateSshKeys(true);
    SshHook{}.checkUserHasSshKeys();
    helper.setRootIds();
    helper.checkHostHasSshKeys();

    // start sshd
    helper.writeContainerStateToStdin();
    SshHook{}.startSshDaemon();
    helper.checkContainerHasEnvironmentFile();
    helper.checkContainerHasEtcProfileModule();
}

TEST(SSHHookTestGroup, testInjectKeyUsingAnnotations) {

    Helper helper{};

    helper.setRootIds();
    helper.setupTestEnvironment();

    // generate + check SSH keys in local repository
    helper.setUserIds(); // keygen is executed with user privileges
    SshHook{}.generateSshKeys(true);
    helper.generateUserSshKeyFile();

    helper.setRootIds();
    helper.checkHostHasSshKeys();

    // start sshd
    helper.writeContainerStateToStdin();
    SshHook{}.startSshDaemon();
    helper.checkContainerHasClientKeys();
    helper.checkContainerHasServerKeys();
    
    CHECK_TRUE(helper.isUserSshKeyAuthorized());
}

}}}} // namespace

SARUS_UNITTEST_MAIN_FUNCTION();
