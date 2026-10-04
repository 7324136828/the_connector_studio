#pragma once
#include <string>
#include <vector>

namespace Lattice
{
    enum class ItemCategory { Files, Sessions, Projects };

    struct ItemType
    {
        std::wstring id;
        std::wstring name;
        std::wstring description;
        std::wstring extension;
        ItemCategory category = ItemCategory::Files;
    };

    struct NewItemRequest
    {
        ItemCategory category = ItemCategory::Projects;
        std::wstring typeId;
        std::wstring name;
        std::wstring location;
    };

    // Declarative, versioned type definitions. Registration never executes code.
    // Future register-file/session/project skills can call RegisterItemType on the controller.
    class ItemTypeRegistry
    {
    public:
        ItemTypeRegistry();
        const std::vector<ItemType>& Types() const { return m_types; }
        const ItemType* Find(const std::wstring& id) const;
        bool RegisterType(const ItemType& type, std::wstring& error);
        bool Save(const std::wstring& path, std::wstring& error) const;
        bool Load(const std::wstring& path, std::wstring& error);

    private:
        std::vector<ItemType> m_types;
    };
}
