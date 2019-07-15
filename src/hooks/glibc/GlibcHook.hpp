/*
 * Sarus
 *
 * Copyright (c) 2018-2019, ETH Zurich. All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef sarus_hooks_glibc_GlibcHook_hpp
#define sarus_hooks_glibc_GlibcHook_hpp

#include <vector>
#include <boost/optional.hpp>
#include <boost/filesystem.hpp>
#include <sys/types.h>

namespace sarus {
namespace hooks {
namespace glibc {

class GlibcHook {
public:
    void injectGlibcLibrariesIfNecessary();

private:
    void parseConfigJSONOfBundle();
    void parseEnvironmentVariables();
    bool containerHasGlibc() const;
    std::vector<boost::filesystem::path> get64bitContainerLibraries() const;
    boost::optional<boost::filesystem::path> findLibc(const std::vector<boost::filesystem::path>& libs) const;
    bool containerGlibcIsNewerOrSame(
        const boost::filesystem::path& hostLibc,
        const boost::filesystem::path& containerLibc) const;
    void verifyThatHostAndContainerGlibcAreABICompatible(
        const boost::filesystem::path& hostLibc,
        const boost::filesystem::path& containerLibc) const;
    void replaceGlibcLibrariesInContainer() const;

private:
    boost::filesystem::path bundleDir;
    boost::filesystem::path rootfsDir;
    pid_t pidOfContainer;
    boost::filesystem::path ldconfigPath;
    boost::filesystem::path readelfPath;
    std::vector<boost::filesystem::path> hostLibraries;
    std::vector<boost::filesystem::path> containerLibraries;
};

}}} // namespace

#endif