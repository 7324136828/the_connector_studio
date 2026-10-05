#include "ChatCopy.h"
#include <stdexcept>

namespace Lattice::ChatCopy
{
    namespace
    {
        constexpr std::size_t MaxCharacters = 32 * 1024 * 1024;
        void Append(std::wstring& destination, const std::wstring& source)
        {
            for (std::size_t index = 0; index < source.size(); ++index)
            {
                const auto character = source[index];
                if (!character) throw std::runtime_error("null");
                if (destination.size() >= MaxCharacters - 2) throw std::length_error("size");
                if (character == L'\r' || character == L'\n')
                {
                    if (character == L'\r' && index + 1 < source.size() && source[index + 1] == L'\n') ++index;
                    destination += L"\r\n";
                }
                else destination += character;
            }
        }
        void AppendBody(std::wstring& destination, const MessageItem& message)
        {
            Append(destination, message.text);
            if (!message.items.empty())
            {
                if (!message.text.empty()) Append(destination, L"\n\n");
                for (std::size_t index = 0; index < message.items.size(); ++index)
                {
                    if (index) Append(destination, L"\n");
                    Append(destination, std::to_wstring(index + 1) + L". ");
                    Append(destination, message.items[index]);
                }
            }
            if (!message.planSteps.empty())
            {
                if (!message.text.empty() || !message.items.empty()) Append(destination, L"\n\n");
                for (std::size_t index = 0; index < message.planSteps.size(); ++index)
                {
                    const auto& step = message.planSteps[index];
                    if (index) Append(destination, L"\n\n");
                    Append(destination, std::to_wstring(step.number) + L". ");
                    Append(destination, step.title);
                    if (!step.description.empty()) { Append(destination, L"\n"); Append(destination, step.description); }
                }
            }
        }
        template<class Formatter>
        Result Format(Formatter formatter)
        {
            Result result;
            try
            {
                formatter(result.text);
                result.success = !result.text.empty();
                if (!result.success) result.error = L"There is no chat text to copy.";
            }
            catch (const std::length_error&) { result.text.clear(); result.error = L"This chat is too large to copy at once. Copy an individual message instead."; }
            catch (...) { result.text.clear(); result.error = L"The chat could not be prepared for the clipboard."; }
            return result;
        }
    }
    bool HasContent(const MessageItem& message)
    { return !message.text.empty() || !message.items.empty() || !message.planSteps.empty(); }
    Result Message(const MessageItem& message)
    { return Format([&](auto& text) { if (HasContent(message)) AppendBody(text, message); }); }
    Result Transcript(const SessionTab& session)
    {
        return Format([&](auto& text)
        {
            for (const auto& message : session.messages)
            {
                if (!HasContent(message)) continue;
                if (!text.empty()) Append(text, L"\n\n");
                Append(text, message.author.empty() ? (message.role == L"user" ? L"You" : L"Assistant") : message.author);
                Append(text, L":\n");
                AppendBody(text, message);
            }
        });
    }
}
