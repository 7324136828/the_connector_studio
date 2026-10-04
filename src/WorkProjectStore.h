#pragma once
#include "ItemTypeRegistry.h"
#include "AppTypes.h"
#include <memory>

namespace Lattice::WorkProjectStore
{
    struct CreationRecord;
    struct ProjectInfo
    {
        std::wstring name;
        std::wstring typeId;
        std::wstring rootPath;
        bool managed = false;
        std::vector<std::wstring> sessionPaths;
    };
    struct CreatedItem
    {
        ItemCategory category = ItemCategory::Files;
        std::wstring path;
        std::wstring metadataPath;
        std::wstring projectRoot;
        // Opaque ownership record prevents rollback from targeting existing items.
        std::shared_ptr<const CreationRecord> ownership;
    };
    bool ValidateName(const std::wstring& name, std::wstring& error);
    bool Open(const std::wstring& path, ProjectInfo& project, std::wstring& error);
    bool Create(const NewItemRequest& request, const ItemType& type, CreatedItem& item,
        std::wstring& error, const SessionTab* sessionDefaults = nullptr);
    bool RollbackCreated(CreatedItem& item, std::wstring& error);
}
