/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KICAD_API_HANDLER_SCH_H
#define KICAD_API_HANDLER_SCH_H

#include <google/protobuf/empty.pb.h>

#include <api/api_handler_editor.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/schematic/schematic_commands.pb.h>
#include <kiid.h>

using namespace kiapi;
using namespace kiapi::common;
using google::protobuf::Empty;

class SCH_EDIT_FRAME;
class SCH_ITEM;


class API_HANDLER_SCH : public API_HANDLER_EDITOR
{
public:
    API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame );

protected:
    std::unique_ptr<COMMIT> createCommit() override;

    kiapi::common::types::DocumentType thisDocumentType() const override
    {
        return kiapi::common::types::DOCTYPE_SCHEMATIC;
    }

    bool validateDocumentInternal( const DocumentSpecifier& aDocument ) const override;

    HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> createItemForType( KICAD_T aType,
                                                                 EDA_ITEM* aContainer );

    HANDLER_RESULT<types::ItemRequestStatus> handleCreateUpdateItemsInternal( bool aCreate,
            const std::string& aClientName,
            const types::ItemHeader &aHeader,
            const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
            std::function<void(commands::ItemStatus, google::protobuf::Any)> aItemHandler )
            override;

    void deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                              const std::string& aClientName ) override;

    std::optional<EDA_ITEM*> getItemFromDocument( const DocumentSpecifier& aDocument,
                                                  const KIID& aId ) override;

private:
    HANDLER_RESULT<commands::GetOpenDocumentsResponse> handleGetOpenDocuments(
            const HANDLER_CONTEXT<commands::GetOpenDocuments>& aCtx );

    HANDLER_RESULT<Empty> handleSaveDocument( const HANDLER_CONTEXT<commands::SaveDocument>& aCtx );

    // Proof of concept handlers
    HANDLER_RESULT<schematic::commands::SchematicInfoResponse> handleGetSchematicInfo(
            const HANDLER_CONTEXT<schematic::commands::GetSchematicInfo>& aCtx );
    
    HANDLER_RESULT<schematic::commands::GetSchematicItemsResponse> handleGetSchematicItems(
            const HANDLER_CONTEXT<schematic::commands::GetSchematicItems>& aCtx );
    
    HANDLER_RESULT<schematic::commands::CreateSchematicItemsResponse> handleCreateSchematicItems(
            const HANDLER_CONTEXT<schematic::commands::CreateSchematicItems>& aCtx );
    
    HANDLER_RESULT<commands::DeleteItemsResponse> handleDeleteItems(
            const HANDLER_CONTEXT<commands::DeleteItems>& aCtx );
    
    // Phase 1A handlers
    HANDLER_RESULT<schematic::commands::DrawWireResponse> handleDrawWire(
            const HANDLER_CONTEXT<schematic::commands::DrawWire>& aCtx );
    
    HANDLER_RESULT<schematic::commands::GetSymbolPinsResponse> handleGetSymbolPins(
            const HANDLER_CONTEXT<schematic::commands::GetSymbolPins>& aCtx );

    // Symbol Placement System - Phase 2 API Handlers
    HANDLER_RESULT<schematic::commands::GetSymbolLibrariesResponse> handleGetSymbolLibraries(
            const HANDLER_CONTEXT<schematic::commands::GetSymbolLibraries>& aCtx );

    HANDLER_RESULT<schematic::commands::SearchSymbolsResponse> handleSearchSymbols(
            const HANDLER_CONTEXT<schematic::commands::SearchSymbols>& aCtx );

    HANDLER_RESULT<schematic::commands::PlaceSymbolResponse> handlePlaceSymbol(
            const HANDLER_CONTEXT<schematic::commands::PlaceSymbol>& aCtx );

    // Library Management APIs - Preloading and refresh support
    HANDLER_RESULT<schematic::commands::PreloadSymbolLibrariesResponse> handlePreloadSymbolLibraries(
            const HANDLER_CONTEXT<schematic::commands::PreloadSymbolLibraries>& aCtx );

    HANDLER_RESULT<schematic::commands::GetLibraryLoadStatusResponse> handleGetLibraryLoadStatus(
            const HANDLER_CONTEXT<schematic::commands::GetLibraryLoadStatus>& aCtx );

    HANDLER_RESULT<schematic::commands::RefreshSymbolLibrariesResponse> handleRefreshSymbolLibraries(
            const HANDLER_CONTEXT<schematic::commands::RefreshSymbolLibraries>& aCtx );

    HANDLER_RESULT<schematic::commands::GetComponentBoundsResponse> handleGetComponentBounds(
            const HANDLER_CONTEXT<schematic::commands::GetComponentBounds>& aCtx );
    
    HANDLER_RESULT<schematic::commands::GetGridAnchorsResponse> handleGetGridAnchors(
            const HANDLER_CONTEXT<schematic::commands::GetGridAnchors>& aCtx );
    
    HANDLER_RESULT<schematic::commands::GetConnectionPointsResponse> handleGetConnectionPoints(
            const HANDLER_CONTEXT<schematic::commands::GetConnectionPoints>& aCtx );

    // Selection Management System - Phase 1 Foundational Optimizations
    HANDLER_RESULT<schematic::commands::SelectionResponse> handleGetSelection(
            const HANDLER_CONTEXT<schematic::commands::GetSelection>& aCtx );
    
    HANDLER_RESULT<Empty> handleClearSelection(
            const HANDLER_CONTEXT<schematic::commands::ClearSelection>& aCtx );
    
    HANDLER_RESULT<schematic::commands::SelectionResponse> handleAddToSelection(
            const HANDLER_CONTEXT<schematic::commands::AddToSelection>& aCtx );
    
    HANDLER_RESULT<schematic::commands::SelectionResponse> handleRemoveFromSelection(
            const HANDLER_CONTEXT<schematic::commands::RemoveFromSelection>& aCtx );

    SCH_EDIT_FRAME* m_frame;
};


#endif //KICAD_API_HANDLER_SCH_H
