#ifndef sarus_runtime_UserMount_hpp
#define sarus_runtime_UserMount_hpp

#include <memory>
#include <sys/mount.h>
#include <boost/filesystem.hpp>

#include "common/Config.hpp"
#include "runtime/Mount.hpp"


namespace sarus {
namespace runtime {

class UserMount : public Mount {
public:
    UserMount(  const boost::filesystem::path& source,
                const boost::filesystem::path& destination,
                const unsigned long mountFlags,
                std::shared_ptr<const common::Config> config);

    void performMount() const override;

public: // public for test purpose
    boost::filesystem::path source;
    boost::filesystem::path destination;
    unsigned long mountFlags;

private:
    std::shared_ptr<const common::Config> config;
};

}
}

#endif
