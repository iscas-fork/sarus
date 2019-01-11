#include "CommandObjectsFactory.hpp"

#include "cli/CommandHelp.hpp"
#include "cli/CommandHelpOfCommand.hpp"
#include "cli/CommandImages.hpp"
#include "cli/CommandLoad.hpp"
#include "cli/CommandPull.hpp"
#include "cli/CommandRmi.hpp"
#include "cli/CommandRun.hpp"
#include "cli/CommandSshKeygen.hpp"
#include "cli/CommandVersion.hpp"


namespace sarus {
namespace cli {

CommandObjectsFactory::CommandObjectsFactory() {
    addCommand<cli::CommandHelp>("help");
    addCommand<cli::CommandImages>("images");
    addCommand<cli::CommandLoad>("load");
    addCommand<cli::CommandPull>("pull");
    addCommand<cli::CommandRmi>("rmi");
    addCommand<cli::CommandRun>("run");
    addCommand<cli::CommandSshKeygen>("ssh-keygen");
    addCommand<cli::CommandVersion>("version");
}

bool CommandObjectsFactory::isValidCommandName(const std::string& commandName) const {
    return map.find(commandName) != map.cend();
}

std::vector<std::string> CommandObjectsFactory::getCommandNames() const {
    auto names = std::vector<std::string>{};
    names.reserve(map.size());
    for(const auto& kv : map) {
        names.push_back(kv.first);
    }
    return names;
}

std::unique_ptr<cli::Command> CommandObjectsFactory::makeCommandObject(const std::string& commandName) const {
    if(!isValidCommandName(commandName)) {
        auto message = boost::format("Failed to make command object for command name \"%s\" (invalid command name)")
            % commandName;
        SARUS_THROW_ERROR(message.str());
    }
    auto it = map.find(commandName);
    return it->second();
}

std::unique_ptr<cli::Command> CommandObjectsFactory::makeCommandObject(
    const std::string& commandName,
    const std::deque<common::CLIArguments>& commandArgsGroups,
    std::shared_ptr<common::Config> config) const {
    if(!isValidCommandName(commandName)) {
        auto message = boost::format("Failed to make command object for command name \"%s\" (invalid command name)")
            % commandName;
        SARUS_THROW_ERROR(message.str());
    }
    auto it = mapWithArguments.find(commandName);
    return it->second(commandArgsGroups, std::move(config));
}

std::unique_ptr<cli::Command> CommandObjectsFactory::makeCommandObjectHelpOfCommand(const std::string& commandName) const {
    auto commandObject = makeCommandObject(commandName);
    auto ptr = new cli::CommandHelpOfCommand{std::move(commandObject)};
    return std::unique_ptr<cli::Command>{ptr};
}

}
}
