#pragma once

#include <string>
#include <vector>

bool parseCallAtOffset(const std::string &text, size_t cursorOffset,
                       std::string &functionNameOut, int &activeParameterOut);

bool extractFunctionSignatureAt(const std::string &text, int lineIndex,
                                int nameCharacter, const std::string &name,
                                std::string &labelOut,
                                std::vector<std::string> &parametersOut);
