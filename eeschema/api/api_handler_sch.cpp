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

#include <api/api_handler_sch.h>
#include <api/api_sch_utils.h>
#include <api/api_utils.h>
#include <magic_enum.hpp>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_item.h>
#include <sch_symbol.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_line.h>
#include <wx/filename.h>

#include <api/common/types/base_types.pb.h>
#include <api/schematic/schematic_commands.pb.h>
#include <api/schematic/schematic_types.pb.h>

using namespace kiapi::common::commands;
using kiapi::common::types::CommandStatus;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;


API_HANDLER_SCH::API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame ) :
        API_HANDLER_EDITOR(),
        m_frame( aFrame )
{
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_SCH::handleGetOpenDocuments );
    
    // Proof of concept handlers
    registerHandler<schematic::commands::GetSchematicInfo, 
                    schematic::commands::SchematicInfoResponse>(
            &API_HANDLER_SCH::handleGetSchematicInfo );
    
    registerHandler<schematic::commands::GetSchematicItems,
                    schematic::commands::GetSchematicItemsResponse>(
            &API_HANDLER_SCH::handleGetSchematicItems );
    
    registerHandler<schematic::commands::CreateSchematicItems,
                    schematic::commands::CreateSchematicItemsResponse>(
            &API_HANDLER_SCH::handleCreateSchematicItems );
    
    // Phase 1A handlers
    registerHandler<schematic::commands::DrawWire,
                    schematic::commands::DrawWireResponse>(
            &API_HANDLER_SCH::handleDrawWire );
}


std::unique_ptr<COMMIT> API_HANDLER_SCH::createCommit()
{
    return std::make_unique<SCH_COMMIT>( m_frame );
}


bool API_HANDLER_SCH::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_SCHEMATIC )
        return false;

    // TODO(JE) need serdes for SCH_SHEET_PATH <> SheetPath
    return true;

    //wxString currentPath = m_frame->GetCurrentSheet().PathAsString();
    //return 0 == aDocument.sheet_path().compare( currentPath.ToStdString() );
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_SCH::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_SCHEMATIC )
    {
        ApiResponseStatus e;

        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    common::types::DocumentSpecifier doc;

    wxFileName fn( m_frame->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_SCHEMATIC );
    doc.set_board_filename( fn.GetFullName() );

    response.mutable_documents()->Add( std::move( doc ) );
    return response;
}


HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> API_HANDLER_SCH::createItemForType( KICAD_T aType,
        EDA_ITEM* aContainer )
{
    if( !aContainer )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Tried to create an item in a null container" );
        return tl::unexpected( e );
    }

    if( aType == SCH_PIN_T && !dynamic_cast<SCH_SYMBOL*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a pin in {}, which is not a symbol",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }
    else if( aType == SCH_SYMBOL_T && !dynamic_cast<SCHEMATIC*>( aContainer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create a symbol in {}, which is not a "
                                          "schematic",
                                          aContainer->GetFriendlyName().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<EDA_ITEM> created = CreateItemForType( aType, aContainer );

    if( !created )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Tried to create an item of type {}, which is unhandled",
                                          magic_enum::enum_name( aType ) ) );
        return tl::unexpected( e );
    }

    return created;
}


HANDLER_RESULT<ItemRequestStatus> API_HANDLER_SCH::handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( ItemStatus, google::protobuf::Any )> aItemHandler )
{
    ApiResponseStatus e;

    auto containerResult = validateItemHeaderDocument( aHeader );

    if( !containerResult && containerResult.error().status() == ApiStatusCode::AS_UNHANDLED )
    {
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }
    else if( !containerResult )
    {
        e.CopyFrom( containerResult.error() );
        return tl::unexpected( e );
    }

    SCH_SCREEN* screen = m_frame->GetScreen();
    EE_RTREE& screenItems = screen->Items();

    std::map<KIID, EDA_ITEM*> itemUuidMap;

    std::for_each( screenItems.begin(), screenItems.end(),
                   [&]( EDA_ITEM* aItem )
                   {
                       itemUuidMap[aItem->m_Uuid] = aItem;
                   } );

    EDA_ITEM* container = nullptr;

    if( containerResult->has_value() )
    {
        const KIID& containerId = **containerResult;

        if( itemUuidMap.count( containerId ) )
        {
            container = itemUuidMap.at( containerId );

            if( !container )
            {
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format(
                        "The requested container {} is not a valid schematic item container",
                        containerId.AsStdString() ) );
                return tl::unexpected( e );
            }
        }
        else
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format(
                    "The requested container {} does not exist in this document",
                    containerId.AsStdString() ) );
            return tl::unexpected( e );
        }
    }

    COMMIT* commit = getCurrentCommit( aClientName );

    for( const google::protobuf::Any& anyItem : aItems )
    {
        ItemStatus status;
        std::optional<KICAD_T> type = TypeNameFromAny( anyItem );

        if( !type )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( fmt::format( "Could not decode a valid type from {}",
                                                   anyItem.type_url() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        HANDLER_RESULT<std::unique_ptr<EDA_ITEM>> creationResult =
                createItemForType( *type, container );

        if( !creationResult )
        {
            status.set_code( ItemStatusCode::ISC_INVALID_TYPE );
            status.set_error_message( creationResult.error().error_message() );
            aItemHandler( status, anyItem );
            continue;
        }

        std::unique_ptr<EDA_ITEM> item( std::move( *creationResult ) );

        if( !item->Deserialize( anyItem ) )
        {
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack {} from request",
                                              item->GetClass().ToStdString() ) );
            return tl::unexpected( e );
        }

        if( aCreate && itemUuidMap.count( item->m_Uuid ) )
        {
            status.set_code( ItemStatusCode::ISC_EXISTING );
            status.set_error_message( fmt::format( "an item with UUID {} already exists",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }
        else if( !aCreate && !itemUuidMap.count( item->m_Uuid ) )
        {
            status.set_code( ItemStatusCode::ISC_NONEXISTENT );
            status.set_error_message( fmt::format( "an item with UUID {} does not exist",
                                                   item->m_Uuid.AsStdString() ) );
            aItemHandler( status, anyItem );
            continue;
        }

        status.set_code( ItemStatusCode::ISC_OK );
        google::protobuf::Any newItem;

        if( aCreate )
        {
            item->Serialize( newItem );
            commit->Add( item.release(), screen );

            if( !m_activeClients.count( aClientName ) )
                pushCurrentCommit( aClientName, _( "Added items via API" ) );
        }
        else
        {
            EDA_ITEM* edaItem = itemUuidMap[item->m_Uuid];

            if( SCH_ITEM* schItem = dynamic_cast<SCH_ITEM*>( edaItem ) )
            {
                schItem->SwapItemData( static_cast<SCH_ITEM*>( item.get() ) );
                schItem->Serialize( newItem );
                commit->Modify( schItem, screen );
            }
            else
            {
                wxASSERT( false );
            }

            if( !m_activeClients.count( aClientName ) )
                pushCurrentCommit( aClientName, _( "Created items via API" ) );
        }

        aItemHandler( status, newItem );
    }


    return ItemRequestStatus::IRS_OK;
}


void API_HANDLER_SCH::deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                           const std::string& aClientName )
{
    // TODO - POC implementation
    SCH_COMMIT commit( m_frame );
    SCH_SCREEN* screen = m_frame->GetScreen();
    
    for( auto& [id, status] : aItemsToDelete )
    {
        EDA_ITEM* item = screen->GetItem( id );
        if( item )
        {
            commit.Remove( item, screen );
            status = ItemDeletionStatus::IDS_OK;
        }
        else
        {
            status = ItemDeletionStatus::IDS_NONEXISTENT;
        }
    }
    
    if( !commit.Empty() )
        commit.Push( _( "Delete items via API" ) );
}


std::optional<EDA_ITEM*> API_HANDLER_SCH::getItemFromDocument( const DocumentSpecifier& aDocument,
                                                               const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    // POC implementation - search through all sheets
    SCHEMATIC& schematic = m_frame->Schematic();
    SCH_SHEET_LIST sheets = schematic.Hierarchy();
    
    for( const SCH_SHEET_PATH& sheet : sheets )
    {
        SCH_SCREEN* screen = sheet.LastScreen();
        if( EDA_ITEM* item = screen->GetItem( aId ) )
            return item;
    }

    return std::nullopt;
}


// Proof of concept implementations
HANDLER_RESULT<schematic::commands::SchematicInfoResponse> 
API_HANDLER_SCH::handleGetSchematicInfo( const HANDLER_CONTEXT<schematic::commands::GetSchematicInfo>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::SchematicInfoResponse response;
    SCHEMATIC& schematic = m_frame->Schematic();
    
    // Project info
    response.set_project_name( schematic.Prj().GetProjectName().ToStdString() );
    
    // Sheet hierarchy info
    SCH_SHEET_LIST sheets = schematic.Hierarchy();
    response.set_sheet_count( sheets.size() );
    
    // Count symbols and collect sheet names
    int symbolCount = 0;
    std::set<wxString> netNames;
    
    for( const SCH_SHEET_PATH& sheet : sheets )
    {
        SCH_SHEET* lastSheet = sheet.Last();
        if( lastSheet )
            response.add_sheet_names( lastSheet->GetName().ToStdString() );
        
        SCH_SCREEN* screen = sheet.LastScreen();
        if( !screen )
            continue;
            
        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->Type() == SCH_SYMBOL_T )
                symbolCount++;
                
            // Collect net names from labels
            if( item->Type() == SCH_LABEL_T || 
                item->Type() == SCH_GLOBAL_LABEL_T ||
                item->Type() == SCH_HIER_LABEL_T )
            {
                SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );
                netNames.insert( label->GetText() );
            }
        }
    }
    
    response.set_symbol_count( symbolCount );
    response.set_net_count( netNames.size() );
    
    return response;
}


HANDLER_RESULT<schematic::commands::GetSchematicItemsResponse>
API_HANDLER_SCH::handleGetSchematicItems( const HANDLER_CONTEXT<schematic::commands::GetSchematicItems>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::GetSchematicItemsResponse response;
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = currentSheet.LastScreen();
    
    if( !screen )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "No active schematic screen" );
        return tl::unexpected( e );
    }
    
    int count = 0;
    for( SCH_ITEM* item : screen->Items() )
    {
        // Only handle simple items for POC
        if( item->Type() == SCH_JUNCTION_T ||
            item->Type() == SCH_LINE_T ||
            item->Type() == SCH_LABEL_T )
        {
            google::protobuf::Any any;
            item->Serialize( any );
            response.add_items()->CopyFrom( any );
            count++;
        }
    }
    
    response.set_total_count( count );
    return response;
}


HANDLER_RESULT<schematic::commands::CreateSchematicItemsResponse>
API_HANDLER_SCH::handleCreateSchematicItems( const HANDLER_CONTEXT<schematic::commands::CreateSchematicItems>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::CreateSchematicItemsResponse response;
    
    // Create commit for undo/redo
    SCH_COMMIT commit( m_frame );
    SCH_SCREEN* screen = m_frame->GetScreen();
    
    if( !screen )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "No active schematic screen" );
        return tl::unexpected( e );
    }
    
    for( const google::protobuf::Any& anyItem : aCtx.Request.items() )
    {
        std::unique_ptr<SCH_ITEM> newItem;
        
        // Determine type and create
        if( anyItem.Is<schematic::types::Junction>() )
        {
            newItem = std::make_unique<SCH_JUNCTION>();
        }
        else if( anyItem.Is<schematic::types::Wire>() )
        {
            newItem = std::make_unique<SCH_LINE>();
            static_cast<SCH_LINE*>( newItem.get() )->SetLayer( LAYER_WIRE );
        }
        else if( anyItem.Is<schematic::types::LocalLabel>() )
        {
            newItem = std::make_unique<SCH_LABEL>();
        }
        else
        {
            response.add_errors( fmt::format( "Unsupported item type: {}", 
                                             anyItem.type_url() ) );
            continue;
        }
        
        if( newItem )
        {
            // Deserialize the item
            if( !newItem->Deserialize( anyItem ) )
            {
                response.add_errors( fmt::format( "Failed to deserialize item of type {}", 
                                                 anyItem.type_url() ) );
                continue;
            }
            
            // Add to screen and commit
            screen->Append( newItem.get() );
            commit.Add( newItem.get(), screen );
            
            // Add the created ID to response
            kiapi::common::types::KIID* createdId = response.add_created_ids();
            createdId->set_value( newItem->m_Uuid.AsStdString() );
            
            // Release ownership to the screen
            newItem.release();
        }
    }
    
    // Push the commit if we created anything
    if( response.created_ids_size() > 0 )
    {
        commit.Push( _( "Create schematic items via API" ) );
        
        // Update connectivity
        m_frame->RecalculateConnections( nullptr, NO_CLEANUP );
    }
    
    return response;
}


HANDLER_RESULT<schematic::commands::DrawWireResponse>
API_HANDLER_SCH::handleDrawWire( const HANDLER_CONTEXT<schematic::commands::DrawWire>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::DrawWireResponse response;
    
    // Get the screen
    SCH_SCREEN* screen = m_frame->GetScreen();
    if( !screen )
    {
        response.set_error( "No active schematic screen" );
        return response;
    }
    
    // Create commit for undo/redo
    SCH_COMMIT commit( m_frame );
    
    // Create a new wire (SCH_LINE)
    std::unique_ptr<SCH_LINE> wire = std::make_unique<SCH_LINE>();
    
    // Set wire properties
    wire->SetLayer( LAYER_WIRE );
    
    // Convert from API coordinates to internal units
    VECTOR2I startPos( aCtx.Request.start_point().x_nm(), aCtx.Request.start_point().y_nm() );
    VECTOR2I endPos( aCtx.Request.end_point().x_nm(), aCtx.Request.end_point().y_nm() );
    
    wire->SetStartPoint( startPos );
    wire->SetEndPoint( endPos );
    
    // Set wire width if specified (0 = use default)
    if( aCtx.Request.width() > 0 )
    {
        wire->SetLineWidth( aCtx.Request.width() );
    }
    
    // Add to screen and commit
    screen->Append( wire.get() );
    commit.Add( wire.get(), screen );
    
    // Set the created wire ID in response
    kiapi::common::types::KIID* wireId = response.mutable_wire_id();
    wireId->set_value( wire->m_Uuid.AsStdString() );
    
    // Release ownership to the screen
    wire.release();
    
    // Push the commit
    commit.Push( _( "Draw wire via API" ) );
    
    // Update connectivity
    m_frame->RecalculateConnections( nullptr, NO_CLEANUP );
    
    return response;
}
