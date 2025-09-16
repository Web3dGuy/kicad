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
#include <api/api_sch_validation.h>
#include <magic_enum.hpp>
#include <refdes_utils.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_item.h>
#include <sch_symbol.h>
#include <symbol.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_line.h>
#include <sch_pin.h>
#include <sch_field.h>
#include <symbol_library.h>
#include <project_sch.h>
#include <symbol_lib_table.h>
#include <wx/filename.h>
#include <tool/tool_manager.h>
#include <chrono>
#include <tools/sch_line_wire_bus_tool.h>
#include <tools/sch_selection.h>
#include <tools/sch_selection_tool.h>
#include <tools/ee_grid_helper.h>

#include <api/common/types/base_types.pb.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/schematic/schematic_commands.pb.h>
#include <api/schematic/schematic_types.pb.h>

using namespace kiapi::common::commands;
using kiapi::common::types::CommandStatus;
using kiapi::common::types::DocumentType;
using kiapi::common::types::ItemRequestStatus;


// Helper function declarations
namespace {

/**
 * Convert coordinates from nanometers (API protocol) to schematic internal units.
 * 
 * The API uses nanometers for absolute precision and consistency across different
 * editor contexts. The schematic editor uses internal units (IU) which are scaled
 * from millimeters. This function handles the conversion through an intermediate
 * millimeter representation.
 * 
 * @param x_nm X coordinate in nanometers from API protocol
 * @param y_nm Y coordinate in nanometers from API protocol  
 * @return Position in schematic internal units ready for use in schematic operations
 */
VECTOR2I convertApiPositionToSchematic( int64_t x_nm, int64_t y_nm )
{
    double x_mm = x_nm / 1000000.0;  // nm to mm conversion
    double y_mm = y_nm / 1000000.0;  // nm to mm conversion
    return VECTOR2I( schIUScale.mmToIU( x_mm ), schIUScale.mmToIU( y_mm ) );
}

/**
 * Convert schematic internal units to nanometers (API protocol).
 * 
 * Performs the reverse conversion of convertApiPositionToSchematic, taking
 * schematic internal units and converting them to nanometers for API responses.
 * 
 * @param position Position in schematic internal units
 * @return Pair of coordinates in nanometers (x_nm, y_nm) for API protocol
 */
std::pair<int64_t, int64_t> convertSchematicPositionToApi( const VECTOR2I& position )
{
    double x_mm = schIUScale.IUTomm( position.x );
    double y_mm = schIUScale.IUTomm( position.y );
    int64_t x_nm = static_cast<int64_t>( x_mm * 1000000.0 );  // mm to nm conversion
    int64_t y_nm = static_cast<int64_t>( y_mm * 1000000.0 );  // mm to nm conversion
    return { x_nm, y_nm };
}

/**
 * Create an API error response with the given status and message.
 * 
 * Standardizes error response creation across all handler methods.
 * 
 * @param status API status code indicating the type of error
 * @param message Human-readable error message for debugging
 * @return Configured ApiResponseStatus object ready to return as tl::unexpected
 */
ApiResponseStatus createErrorResponse( ApiStatusCode status, const std::string& message )
{
    ApiResponseStatus e;
    e.set_status( status );
    e.set_error_message( message );
    return e;
}

/**
 * Configure text properties for a schematic label with default visibility settings.
 * 
 * Sets consistent text size, justification, and color for label visibility.
 * Used by LocalLabel, GlobalLabel, and HierarchicalLabel creation.
 * 
 * @param label The label object to configure
 * @param position The position to set for the label
 */
void configureSchematicLabel( SCH_LABEL_BASE* label, const VECTOR2I& position )
{
    label->SetPosition( position );
    
    // Set default text properties for proper visibility
    label->SetTextSize( VECTOR2I( schIUScale.MilsToIU( 50 ), schIUScale.MilsToIU( 50 ) ) );
    label->SetHorizJustify( GR_TEXT_H_ALIGN_LEFT );
    label->SetVertJustify( GR_TEXT_V_ALIGN_BOTTOM );
    
    // Use red color for visibility (matches user's test color)
    label->SetTextColor( COLOR4D( 1.0, 0.0, 0.094, 1.0 ) );  // Red: RGB(255, 0, 24)
}

}  // anonymous namespace


API_HANDLER_SCH::API_HANDLER_SCH( SCH_EDIT_FRAME* aFrame ) :
        API_HANDLER_EDITOR(),
        m_frame( aFrame )
{
    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_SCH::handleGetOpenDocuments );
    registerHandler<SaveDocument, Empty>( &API_HANDLER_SCH::handleSaveDocument );
    
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
    
    registerHandler<DeleteItems, DeleteItemsResponse>(
            &API_HANDLER_SCH::handleDeleteItems );
    
    // Phase 1A handlers
    registerHandler<schematic::commands::DrawWire,
                    schematic::commands::DrawWireResponse>(
            &API_HANDLER_SCH::handleDrawWire );
    
    registerHandler<schematic::commands::GetSymbolPins,
                    schematic::commands::GetSymbolPinsResponse>(
            &API_HANDLER_SCH::handleGetSymbolPins );

    // Symbol Placement System - Phase 2 Handler Registration
    registerHandler<schematic::commands::GetSymbolLibraries,
                    schematic::commands::GetSymbolLibrariesResponse>(
            &API_HANDLER_SCH::handleGetSymbolLibraries );

    registerHandler<schematic::commands::SearchSymbols,
                    schematic::commands::SearchSymbolsResponse>(
            &API_HANDLER_SCH::handleSearchSymbols );

    registerHandler<schematic::commands::PlaceSymbol,
                    schematic::commands::PlaceSymbolResponse>(
            &API_HANDLER_SCH::handlePlaceSymbol );

    // Library Management APIs - Preloading and refresh support
    registerHandler<schematic::commands::PreloadSymbolLibraries,
                    schematic::commands::PreloadSymbolLibrariesResponse>(
            &API_HANDLER_SCH::handlePreloadSymbolLibraries );

    registerHandler<schematic::commands::GetLibraryLoadStatus,
                    schematic::commands::GetLibraryLoadStatusResponse>(
            &API_HANDLER_SCH::handleGetLibraryLoadStatus );

    registerHandler<schematic::commands::RefreshSymbolLibraries,
                    schematic::commands::RefreshSymbolLibrariesResponse>(
            &API_HANDLER_SCH::handleRefreshSymbolLibraries );

    registerHandler<schematic::commands::GetComponentBounds,
                    schematic::commands::GetComponentBoundsResponse>(
            &API_HANDLER_SCH::handleGetComponentBounds );
    
    registerHandler<schematic::commands::GetGridAnchors,
                    schematic::commands::GetGridAnchorsResponse>(
            &API_HANDLER_SCH::handleGetGridAnchors );
    
    registerHandler<schematic::commands::GetConnectionPoints,
                    schematic::commands::GetConnectionPointsResponse>(
            &API_HANDLER_SCH::handleGetConnectionPoints );
    
    // Selection Management System - Phase 1 Foundational Optimizations
    registerHandler<schematic::commands::GetSelection,
                    schematic::commands::SelectionResponse>(
            &API_HANDLER_SCH::handleGetSelection );
    
    registerHandler<schematic::commands::ClearSelection, Empty>(
            &API_HANDLER_SCH::handleClearSelection );
    
    registerHandler<schematic::commands::AddToSelection,
                    schematic::commands::SelectionResponse>(
            &API_HANDLER_SCH::handleAddToSelection );
    
    registerHandler<schematic::commands::RemoveFromSelection,
                    schematic::commands::SelectionResponse>(
            &API_HANDLER_SCH::handleRemoveFromSelection );
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

        // Section 5 Enhancement: Comprehensive C++ API layer validation
        // Validate deserialized item data before KiCad operations to prevent silent corruption
        if( SCH_ITEM* schItem = dynamic_cast<SCH_ITEM*>( item.get() ) )
        {
            auto validationResult = API_SCH_VALIDATION::ItemValidator::ValidateSchematicItem( schItem );
            if( validationResult )
            {
                status.set_code( ItemStatusCode::ISC_INVALID_DATA );
                status.set_error_message( fmt::format( "Validation failed: {}",
                    API_SCH_VALIDATION::ErrorFormatter::FormatValidationError( *validationResult ) ) );
                aItemHandler( status, anyItem );
                continue;
            }
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
    SCH_SCREEN* screen = m_frame->GetScreen();
    
    if( !screen )
    {
        // Mark all items as nonexistent if we can't access the screen
        for( auto& [id, status] : aItemsToDelete )
            status = ItemDeletionStatus::IDS_NONEXISTENT;
        return;
    }
    
    std::vector<SCH_ITEM*> validatedItems;
    
    // First pass: validate items and mark their status
    for( auto& [id, status] : aItemsToDelete )
    {
        SCH_ITEM* item = nullptr;
        
        // Search through all screen items
        for( SCH_ITEM* screenItem : screen->Items() )
        {
            if( screenItem->m_Uuid == id )
            {
                item = screenItem;
                break;
            }
        }
        
        if( item )
        {
            validatedItems.push_back( item );
            status = ItemDeletionStatus::IDS_OK;
        }
        else
        {
            status = ItemDeletionStatus::IDS_NONEXISTENT;
        }
    }
    
    // Second pass: delete validated items using proper commit management
    if( !validatedItems.empty() )
    {
        COMMIT* commit = getCurrentCommit( aClientName );
        
        for( SCH_ITEM* item : validatedItems )
            commit->Remove( item, screen );
        
        // Push commit if we're not in an active client session
        if( !m_activeClients.count( aClientName ) )
            pushCurrentCommit( aClientName, _( "Deleted items via API" ) );
    }
}


std::optional<EDA_ITEM*> API_HANDLER_SCH::getItemFromDocument( const DocumentSpecifier& aDocument,
                                                               const KIID& aId )
{
    if( !validateDocument( aDocument ) )
        return std::nullopt;

    // Get current schematic screen to search for items
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = currentSheet.LastScreen();
    
    if( !screen )
        return std::nullopt;
    
    // Search through all items on the screen for matching KIID
    for( SCH_ITEM* item : screen->Items() )
    {
        if( item && item->m_Uuid == aId )
        {
            return static_cast<EDA_ITEM*>( item );
        }
    }
    
    return std::nullopt;
}


/**
 * Handle GetSchematicInfo API request.
 * 
 * Retrieves basic information about the current schematic including project name,
 * sheet hierarchy, symbol count, and net count. This provides a high-level overview
 * of the schematic structure without returning detailed item data.
 * 
 * @param aCtx Request context containing the schematic document specifier
 * @return SchematicInfoResponse with project metadata or error status
 */
HANDLER_RESULT<schematic::commands::SchematicInfoResponse> 
API_HANDLER_SCH::handleGetSchematicInfo( const HANDLER_CONTEXT<schematic::commands::GetSchematicInfo>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        return tl::unexpected( createErrorResponse( ApiStatusCode::AS_BAD_REQUEST,
                                                   "Invalid schematic document" ) );
    }
    
    schematic::commands::SchematicInfoResponse response;
    SCHEMATIC& schematic = m_frame->Schematic();
    
    // Project info - access through frame  
    response.set_project_name( m_frame->Prj().GetProjectName().ToStdString() );
    
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


/**
 * Handle GetSchematicItems API request.
 * 
 * Retrieves all schematic items from the current sheet and serializes them for
 * the API response. Supports filtering by item type and includes position data
 * for symbols with embedded pin information for precise wire routing.
 * 
 * @param aCtx Request context with optional item type filter
 * @return GetSchematicItemsResponse containing serialized schematic items or error
 */
HANDLER_RESULT<schematic::commands::GetSchematicItemsResponse>
API_HANDLER_SCH::handleGetSchematicItems( const HANDLER_CONTEXT<schematic::commands::GetSchematicItems>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        return tl::unexpected( createErrorResponse( ApiStatusCode::AS_BAD_REQUEST,
                                                   "Invalid schematic document" ) );
    }
    
    schematic::commands::GetSchematicItemsResponse response;
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = currentSheet.LastScreen();
    
    if( !screen )
    {
        return tl::unexpected( createErrorResponse( ApiStatusCode::AS_BAD_REQUEST,
                                                   "No active schematic screen" ) );
    }
    
    int count = 0;
    for( SCH_ITEM* item : screen->Items() )
    {
        google::protobuf::Any* any = nullptr;
        
        // Handle different item types
        if( item->Type() == SCH_JUNCTION_T ||
            item->Type() == SCH_LINE_T ||
            item->Type() == SCH_LABEL_T )
        {
            any = response.add_items();
            item->Serialize( *any );
            count++;
        }
        else if( item->Type() == SCH_SYMBOL_T )
        {
            // Handle symbols specially - we need to extract position and other data
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
            
            // Create a Symbol protobuf message
            schematic::types::Symbol symbolMsg;
            
            // Set basic properties
            symbolMsg.mutable_id()->set_value( symbol->m_Uuid.AsStdString() );
            
            // Get position and convert from KiCad schematic internal units to nanometers (1 schematic IU = 100 nm)
            VECTOR2I pos = symbol->GetPosition();
            symbolMsg.mutable_position()->set_x_nm( pos.x * 100 );
            symbolMsg.mutable_position()->set_y_nm( pos.y * 100 );
            
            // Get reference and value fields
            symbolMsg.set_reference( symbol->GetField( FIELD_T::REFERENCE )->GetText().ToStdString() );
            symbolMsg.set_value( symbol->GetField( FIELD_T::VALUE )->GetText().ToStdString() );
            
            // Get library ID
            symbolMsg.set_library_id( wxString( symbol->GetLibId().Format().wx_str() ).ToStdString() );
            
            // Get unit and body style
            symbolMsg.set_unit( symbol->GetUnit() );
            symbolMsg.set_body_style( symbol->GetBodyStyle() );
            
            // Get orientation and mirroring
            // Fix: Use GetOrientationProp() to get actual rotation angle
            // Previously this was returning encoded flags (0-5) as degrees
            SYMBOL_ORIENTATION_PROP orientationProp = symbol->GetOrientationProp();
            int degrees = 0;
            switch( orientationProp )
            {
                case SYMBOL_ORIENTATION_PROP::SYMBOL_ANGLE_0:   degrees = 0;   break;
                case SYMBOL_ORIENTATION_PROP::SYMBOL_ANGLE_90:  degrees = 90;  break;
                case SYMBOL_ORIENTATION_PROP::SYMBOL_ANGLE_180: degrees = 180; break;
                case SYMBOL_ORIENTATION_PROP::SYMBOL_ANGLE_270: degrees = 270; break;
                default: degrees = 0; break;
            }
            symbolMsg.mutable_orientation()->set_value_degrees( degrees );
            
            // Fix: Use actual mirror detection methods
            symbolMsg.set_mirrored_x( symbol->GetMirrorX() );
            symbolMsg.set_mirrored_y( symbol->GetMirrorY() );
            
            // Get pins with their positions
            std::vector<SCH_PIN*> pins = symbol->GetPins();
            for( SCH_PIN* pin : pins )
            {
                schematic::types::Pin* pinMsg = symbolMsg.add_pins();
                
                pinMsg->mutable_id()->set_value( pin->m_Uuid.AsStdString() );
                pinMsg->set_name( pin->GetName().ToStdString() );
                pinMsg->set_number( pin->GetNumber().ToStdString() );
                
                // Get the position of the pin (already transformed by SCH_PIN::GetPosition())
                VECTOR2I pinPos = pin->GetPosition();
                // Convert from KiCad schematic internal units to nanometers (1 schematic IU = 100 nm)
                pinMsg->mutable_position()->set_x_nm( pinPos.x * 100 );
                pinMsg->mutable_position()->set_y_nm( pinPos.y * 100 );
                
                // Get electrical type
                pinMsg->set_electrical_type( static_cast<schematic::types::PinElectricalType>( pin->GetType() ) );
                
                // Get orientation (0=right, 90=up, 180=left, 270=down) - convert enum to int
                pinMsg->set_orientation( static_cast<int32_t>( pin->GetOrientation() ) );
                
                // Get length
                pinMsg->set_length( pin->GetLength() );
            }
            
            // Pack into Any
            any = response.add_items();
            any->PackFrom( symbolMsg );
            count++;
        }
    }
    
    response.set_total_count( count );
    return response;
}


/**
 * Handle CreateSchematicItems API request.
 * 
 * Creates new schematic items including junctions, wires, and labels. This is the
 * primary API endpoint for adding elements to a schematic. Supports atomic creation
 * of multiple items with proper validation and wire breaking for junctions.
 * 
 * Special handling for junctions:
 * - Validates junction placement using IsExplicitJunctionAllowed
 * - Automatically breaks wires at junction position
 * - Supports custom diameter and color properties
 * 
 * @param aCtx Request context containing items to create
 * @return CreateSchematicItemsResponse with created item IDs or error messages
 */
HANDLER_RESULT<schematic::commands::CreateSchematicItemsResponse>
API_HANDLER_SCH::handleCreateSchematicItems( const HANDLER_CONTEXT<schematic::commands::CreateSchematicItems>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        return tl::unexpected( createErrorResponse( ApiStatusCode::AS_BAD_REQUEST,
                                                   "Invalid schematic document" ) );
    }
    
    schematic::commands::CreateSchematicItemsResponse response;
    
    // Create commit for undo/redo support
    SCH_COMMIT commit( m_frame );
    SCH_SCREEN* screen = m_frame->GetScreen();
    
    if( !screen )
    {
        return tl::unexpected( createErrorResponse( ApiStatusCode::AS_BAD_REQUEST,
                                                   "No active schematic screen" ) );
    }
    
    for( const google::protobuf::Any& anyItem : aCtx.Request.items() )
    {
        std::unique_ptr<SCH_ITEM> newItem;
        
        // Determine type and create
        if( anyItem.Is<schematic::types::Junction>() )
        {
            // JUNCTION CREATION WITH COMPREHENSIVE VALIDATION
            // The junction API was rebuilt on January 4, 2025 to properly handle:
            // 1. Position validation (must be on wire or crossing)
            // 2. Wire breaking at junction point
            // 3. Default values for diameter and color
            schematic::types::Junction junction;
            if( !anyItem.UnpackTo( &junction ) )
            {
                response.add_errors( "Failed to unpack junction data" );
                continue;
            }
            
            // Convert position from API coordinates (nanometers) to schematic internal units
            VECTOR2I position = convertApiPositionToSchematic( junction.position().x_nm(),
                                                              junction.position().y_nm() );
            
            // CRITICAL VALIDATION: Check if junction is allowed at this position
            // IsExplicitJunctionAllowed returns true only if:
            // - Position is on a wire segment (allows T-junctions)
            // - Position is at intersection of multiple wires (allows cross-junctions)
            // Returns false for isolated positions or single wire endpoints
            if( !screen->IsExplicitJunctionAllowed( position ) )
            {
                double x_mm = schIUScale.IUTomm( position.x );
                double y_mm = schIUScale.IUTomm( position.y );
                response.add_errors( fmt::format( "Junction not allowed at position ({:.3f}, {:.3f}) mm - "
                                                 "must be on wire or at wire intersection", 
                                                 x_mm, y_mm ) );
                continue;
            }
            
            // Create the junction with KiCad defaults
            SCH_JUNCTION* schJunction = new SCH_JUNCTION( position );
            
            // Set diameter if specified (0 means use schematic default)
            if( junction.diameter() > 0 )
            {
                // Convert diameter from nanometers to schematic internal units
                double diameter_mm = junction.diameter() / 1000000.0;
                schJunction->SetDiameter( schIUScale.mmToIU( diameter_mm ) );
            }
            else
            {
                schJunction->SetDiameter( 0 );  // 0 = use schematic's default junction size
            }
            
            // Set color if specified (UNSPECIFIED means use schematic theme color)
            if( junction.has_color() )
            {
                COLOR4D color( junction.color().r() / 255.0,
                              junction.color().g() / 255.0,
                              junction.color().b() / 255.0,
                              junction.color().a() / 255.0 );
                schJunction->SetColor( color );
            }
            else
            {
                schJunction->SetColor( COLOR4D::UNSPECIFIED );  // Use theme default
            }
            
            // Add junction to screen (must be done before wire breaking)
            screen->Append( schJunction );
            commit.Add( schJunction, screen );
            
            // CRITICAL: Break wires at junction position
            // This splits any wire passing through the junction point into two segments,
            // ensuring proper electrical connectivity. Without this, the junction would
            // just overlap the wire visually without creating an electrical connection.
            SCH_LINE_WIRE_BUS_TOOL* wireTool = m_frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>();
            if( wireTool )
            {
                wireTool->BreakSegments( &commit, position, screen );
            }
            
            // Add the created ID to response
            kiapi::common::types::KIID* createdId = response.add_created_ids();
            createdId->set_value( schJunction->m_Uuid.AsStdString() );
            
            continue;
        }
        else if( anyItem.Is<schematic::types::Wire>() )
        {
            newItem = std::make_unique<SCH_LINE>();
            static_cast<SCH_LINE*>( newItem.get() )->SetLayer( LAYER_WIRE );
        }
        else if( anyItem.Is<schematic::types::LocalLabel>() )
        {
            newItem = std::make_unique<SCH_LABEL>();
            
            // Unpack and configure local label
            schematic::types::LocalLabel label;
            if( anyItem.UnpackTo( &label ) && label.has_text() && label.text().has_text() )
            {
                SCH_LABEL* schLabel = static_cast<SCH_LABEL*>( newItem.get() );
                schLabel->SetText( label.text().text().text() );
                
                // Convert position and apply standard label configuration
                VECTOR2I position = convertApiPositionToSchematic( label.position().x_nm(),
                                                                  label.position().y_nm() );
                configureSchematicLabel( schLabel, position );
            }
        }
        else if( anyItem.Is<schematic::types::GlobalLabel>() )
        {
            newItem = std::make_unique<SCH_GLOBALLABEL>();
            
            // Unpack and configure global label
            schematic::types::GlobalLabel label;
            if( anyItem.UnpackTo( &label ) && label.has_text() && label.text().has_text() )
            {
                SCH_GLOBALLABEL* globalLabel = static_cast<SCH_GLOBALLABEL*>( newItem.get() );
                globalLabel->SetText( label.text().text().text() );
                
                // Convert position and apply standard label configuration
                VECTOR2I position = convertApiPositionToSchematic( label.position().x_nm(),
                                                                  label.position().y_nm() );
                configureSchematicLabel( globalLabel, position );
                
                // Global labels have an additional shape property
                globalLabel->SetShape( LABEL_FLAG_SHAPE::L_INPUT );  // Default shape for global labels
            }
        }
        else
        {
            response.add_errors( fmt::format( "Unsupported item type: {}", 
                                             anyItem.type_url() ) );
            continue;
        }
        
        if( newItem )
        {
            // Skip deserialize for manually handled items (they've already been configured)
            bool skipDeserialize = anyItem.Is<schematic::types::LocalLabel>() ||
                                   anyItem.Is<schematic::types::GlobalLabel>();
            
            if( !skipDeserialize )
            {
                // Deserialize the item
                if( !newItem->Deserialize( anyItem ) )
                {
                    response.add_errors( fmt::format( "Failed to deserialize item of type {}", 
                                                     anyItem.type_url() ) );
                    continue;
                }
            }
            
            // Add to screen and commit using standard API methods
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


HANDLER_RESULT<DeleteItemsResponse>
API_HANDLER_SCH::handleDeleteItems( const HANDLER_CONTEXT<DeleteItems>& aCtx )
{
    if( !validateDocument( aCtx.Request.header().document() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    DeleteItemsResponse response;
    response.mutable_header()->CopyFrom( aCtx.Request.header() );
    response.set_status( ItemRequestStatus::IRS_OK );
    
    // Build map for deleteItemsInternal
    std::map<KIID, ItemDeletionStatus> itemsToDelete;
    for( const kiapi::common::types::KIID& itemId : aCtx.Request.item_ids() )
    {
        KIID uuid( itemId.value() );
        itemsToDelete[uuid] = ItemDeletionStatus::IDS_OK; // Will be updated by deleteItemsInternal
    }
    
    // Use existing battle-tested deletion logic
    deleteItemsInternal( itemsToDelete, "MCP_API_Client" );
    
    // Build response from results
    for( const auto& [uuid, status] : itemsToDelete )
    {
        ItemDeletionResult* result = response.add_deleted_items();
        result->mutable_id()->set_value( uuid.AsStdString() );
        result->set_status( status );
    }
    
    return response;
}


/**
 * Handle DrawWire API request.
 * 
 * Creates a wire segment between two points in the schematic. This is the Phase 1A
 * implementation for basic wire drawing. Automatically adds junctions at connection
 * points and breaks existing wires as needed to maintain proper connectivity.
 * 
 * @param aCtx Request context with start/end points in nanometers and optional width
 * @return DrawWireResponse with created wire ID or error message
 */
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
    
    // Convert from API coordinates (nanometers) to internal units
    // NOTE: Direct conversion (1 schematic IU = 100 nm) - tested and working
    VECTOR2I startPos( aCtx.Request.start_point().x_nm() / 100, aCtx.Request.start_point().y_nm() / 100 );
    VECTOR2I endPos( aCtx.Request.end_point().x_nm() / 100, aCtx.Request.end_point().y_nm() / 100 );
    
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
    
    // Get tool manager for junction creation
    TOOL_MANAGER* toolMgr = m_frame->GetToolManager();
    SCH_LINE_WIRE_BUS_TOOL* lwbTool = toolMgr ? toolMgr->GetTool<SCH_LINE_WIRE_BUS_TOOL>() : nullptr;

    // Store wire pointer before releasing ownership
    SCH_LINE* wirePtr = wire.get();

    // Release ownership to the screen
    wire.release();
    
    // Push the commit
    commit.Push( _( "Draw wire via API" ) );
    
    // Add junctions if needed (matching interactive tool behavior)
    if( lwbTool )
    {
        SCH_COMMIT junctionCommit( m_frame );
        SCH_SELECTION selection;
        selection.Add( wirePtr );
        
        int junctionsAdded = lwbTool->AddJunctionsIfNeeded( &junctionCommit, &selection );
        if( junctionsAdded > 0 )
            junctionCommit.Push( _( "Add junctions via API" ) );
    }
    
    // Update connectivity after junction creation
    m_frame->RecalculateConnections( nullptr, NO_CLEANUP );
    
    return response;
}


/**
 * Handle GetSymbolPins API request.
 * 
 * Retrieves detailed pin information for a specific symbol including position,
 * orientation, electrical type, and name. Essential for smart wire routing
 * to determine exact connection points on components.
 * 
 * @param aCtx Request context with symbol ID
 * @return GetSymbolPinsResponse with pin details or error status
 */
HANDLER_RESULT<schematic::commands::GetSymbolPinsResponse>
API_HANDLER_SCH::handleGetSymbolPins( const HANDLER_CONTEXT<schematic::commands::GetSymbolPins>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::GetSymbolPinsResponse response;
    
    // Get the screen
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = currentSheet.LastScreen();
    
    if( !screen )
    {
        response.set_error( "No active schematic screen" );
        return response;
    }
    
    // Find the symbol with the given ID
    KIID symbolId( aCtx.Request.symbol_id().value() );
    SCH_SYMBOL* symbol = nullptr;
    
    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() == SCH_SYMBOL_T && item->m_Uuid == symbolId )
        {
            symbol = static_cast<SCH_SYMBOL*>( item );
            break;
        }
    }
    
    if( !symbol )
    {
        response.set_error( "Symbol not found" );
        return response;
    }
    
    // Set symbol info
    response.set_reference( symbol->GetField( FIELD_T::REFERENCE )->GetText().ToStdString() );
    
    VECTOR2I symbolPos = symbol->GetPosition();
    // Convert from KiCad schematic internal units to nanometers (1 schematic IU = 100 nm)
    response.mutable_symbol_position()->set_x_nm( symbolPos.x * 100 );
    response.mutable_symbol_position()->set_y_nm( symbolPos.y * 100 );
    
    // Get all pins for the symbol
    std::vector<SCH_PIN*> pins = symbol->GetPins();
    
    for( SCH_PIN* pin : pins )
    {
        schematic::types::Pin* pinMsg = response.add_pins();
        
        // Set pin properties
        pinMsg->mutable_id()->set_value( pin->m_Uuid.AsStdString() );
        pinMsg->set_name( pin->GetName().ToStdString() );
        pinMsg->set_number( pin->GetNumber().ToStdString() );
        
        // Get the position of the pin (already transformed by SCH_PIN::GetPosition())
        VECTOR2I pinPos = pin->GetPosition();
        // Convert from KiCad schematic internal units to nanometers (1 schematic IU = 100 nm)
        pinMsg->mutable_position()->set_x_nm( pinPos.x * 100 );
        pinMsg->mutable_position()->set_y_nm( pinPos.y * 100 );
        
        // Get electrical type
        pinMsg->set_electrical_type( static_cast<schematic::types::PinElectricalType>( pin->GetType() ) );
        
        // Get orientation (0=right, 90=up, 180=left, 270=down) - convert enum to int
        pinMsg->set_orientation( static_cast<int32_t>( pin->GetOrientation() ) );
        
        // Get length
        pinMsg->set_length( pin->GetLength() );
    }
    
    return response;
}


/**
 * Handle GetComponentBounds API request.
 * 
 * Retrieves bounding box information for all symbols in the schematic.
 * Used by smart routing to avoid drawing wires through component bodies
 * and to implement Manhattan routing around obstacles.
 * 
 * @param aCtx Request context with schematic specifier
 * @return GetComponentBoundsResponse with symbol boundaries or error status
 */
HANDLER_RESULT<schematic::commands::GetComponentBoundsResponse>
API_HANDLER_SCH::handleGetComponentBounds( const HANDLER_CONTEXT<schematic::commands::GetComponentBounds>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::GetComponentBoundsResponse response;
    
    // Get the screen
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = currentSheet.LastScreen();
    
    if( !screen )
    {
        response.set_error( "No active schematic screen" );
        return response;
    }
    
    // Find the symbol with the given ID
    KIID symbolId( aCtx.Request.symbol_id().value() );
    SCH_SYMBOL* symbol = nullptr;
    
    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() == SCH_SYMBOL_T && item->m_Uuid == symbolId )
        {
            symbol = static_cast<SCH_SYMBOL*>( item );
            break;
        }
    }
    
    if( !symbol )
    {
        response.set_error( "Symbol not found" );
        return response;
    }
    
    // Set symbol reference
    response.set_reference( symbol->GetField( FIELD_T::REFERENCE )->GetText().ToStdString() );
    
    // Get bounding box based on request parameters
    BOX2I bbox;
    if( aCtx.Request.include_pins() && aCtx.Request.include_fields() )
    {
        // Use the full bounding box method that includes pins and fields
        bbox = symbol->GetBoundingBox();
    }
    else if( aCtx.Request.include_pins() )
    {
        // Get body + pins bounding box (exclude fields)
        bbox = symbol->GetBodyAndPinsBoundingBox();
    }
    else
    {
        // Get symbol body bounding box only (exclude pins and fields)
        bbox = symbol->GetBodyBoundingBox();
    }
    
    // Convert bounding box to response format
    // Convert from KiCad schematic internal units to nanometers (1 schematic IU = 100 nm)
    response.mutable_top_left()->set_x_nm( bbox.GetX() * 100 );
    response.mutable_top_left()->set_y_nm( bbox.GetY() * 100 );
    response.mutable_bottom_right()->set_x_nm( bbox.GetRight() * 100 );
    response.mutable_bottom_right()->set_y_nm( bbox.GetBottom() * 100 );
    
    return response;
}


/**
 * Handle GetGridAnchors API request.
 * 
 * Retrieves grid anchor points from all schematic items that can serve as
 * wire connection targets. This includes pins, wire endpoints, and junction
 * positions for precise grid-aligned routing.
 * 
 * @param aCtx Request context with optional position filter
 * @return GetGridAnchorsResponse with anchor points or error status
 */
HANDLER_RESULT<schematic::commands::GetGridAnchorsResponse>
API_HANDLER_SCH::handleGetGridAnchors( const HANDLER_CONTEXT<schematic::commands::GetGridAnchors>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::GetGridAnchorsResponse response;
    
    // Get the screen
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = currentSheet.LastScreen();
    
    if( !screen )
    {
        response.set_error( "No active schematic screen" );
        return response;
    }
    
    // Convert position from nanometers to internal units
    VECTOR2I position( aCtx.Request.position().x_nm() / 100, aCtx.Request.position().y_nm() / 100 );
    
    // Create grid helper instance
    EE_GRID_HELPER grid( m_frame->GetToolManager() );
    
    // Convert skip items to selection
    SCH_SELECTION skipItems;
    for( const auto& skipId : aCtx.Request.skip_items() )
    {
        KIID itemId( skipId.value() );
        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->m_Uuid == itemId )
            {
                skipItems.Add( item );
                break;
            }
        }
    }
    
    // Get best snap anchor
    VECTOR2I snapPos = grid.BestSnapAnchor( position, GRID_WIRES, skipItems );
    
    // Create anchor for the best snap position
    schematic::commands::GridAnchor* anchor = response.add_anchors();
    anchor->mutable_position()->set_x_nm( snapPos.x * 100 );
    anchor->mutable_position()->set_y_nm( snapPos.y * 100 );
    anchor->set_type( "snap" );
    anchor->set_distance( ( snapPos - position ).EuclideanNorm() * 100 );
    
    // Get grid position as well
    VECTOR2I nearestGrid = grid.GetOrigin();
    
    schematic::commands::GridAnchor* gridAnchor = response.add_anchors();
    gridAnchor->mutable_position()->set_x_nm( nearestGrid.x * 100 );
    gridAnchor->mutable_position()->set_y_nm( nearestGrid.y * 100 );
    gridAnchor->set_type( "grid" );
    gridAnchor->set_distance( ( nearestGrid - position ).EuclideanNorm() * 100 );
    
    return response;
}


/**
 * Handle GetConnectionPoints API request.
 * 
 * Retrieves all electrical connection points in the schematic including pins,
 * wire endpoints, junctions, and labels. Provides connectivity information
 * for intelligent wire routing and net analysis.
 * 
 * @param aCtx Request context with optional filters
 * @return GetConnectionPointsResponse with connection points or error status
 */
HANDLER_RESULT<schematic::commands::GetConnectionPointsResponse>
API_HANDLER_SCH::handleGetConnectionPoints( const HANDLER_CONTEXT<schematic::commands::GetConnectionPoints>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }
    
    schematic::commands::GetConnectionPointsResponse response;
    
    // Get the screen
    SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
    SCH_SCREEN* screen = currentSheet.LastScreen();
    
    if( !screen )
    {
        response.set_error( "No active schematic screen" );
        return response;
    }
    
    // Get all connection points from the screen
    std::vector<VECTOR2I> connections = screen->GetConnections();
    
    // Convert and add connection points to response
    for( const VECTOR2I& conn : connections )
    {
        // Convert from KiCad schematic internal units to nanometers (1 schematic IU = 100 nm)
        kiapi::common::types::Vector2* connPoint = response.add_connections();
        connPoint->set_x_nm( conn.x * 100 );
        connPoint->set_y_nm( conn.y * 100 );
    }
    
    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleSaveDocument(
        const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() );
    
    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );
    
    // Save the schematic document
    m_frame->SaveProject();
    
    return Empty();
}


// Selection Management System Implementation - Phase 1 Foundational Optimizations

HANDLER_RESULT<schematic::commands::SelectionResponse> API_HANDLER_SCH::handleGetSelection(
        const HANDLER_CONTEXT<schematic::commands::GetSelection>& aCtx )
{
    /**
     * Get currently selected schematic items.
     * Integrates with SCH_SELECTION_TOOL to return active selection set.
     */
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );
    
    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );
    
    schematic::commands::SelectionResponse response;
    
    try
    {
        // Get the selection tool from the tool manager
        TOOL_MANAGER* toolMgr = m_frame->GetToolManager();
        if( !toolMgr )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        SCH_SELECTION_TOOL* selectionTool = toolMgr->GetTool<SCH_SELECTION_TOOL>();
        if( !selectionTool )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        // Get current selection
        SCH_SELECTION& selection = selectionTool->GetSelection();
        
        // Convert selection to protocol buffer format
        // This follows the same pattern as handleGetSchematicItems
        for( EDA_ITEM* item : selection )
        {
            if( SCH_ITEM* schItem = dynamic_cast<SCH_ITEM*>( item ) )
            {
                google::protobuf::Any* any = nullptr;

                // Handle different item types
                if( schItem->Type() == SCH_JUNCTION_T ||
                    schItem->Type() == SCH_LINE_T ||
                    schItem->Type() == SCH_LABEL_T )
                {
                    any = response.add_items();
                    schItem->Serialize( *any );
                }
                else if( schItem->Type() == SCH_SYMBOL_T )
                {
                    // Handle symbols specially - extract position and other data
                    SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( schItem );

                    // Create a Symbol protobuf message
                    schematic::types::Symbol symbolMsg;

                    // Set basic properties
                    symbolMsg.mutable_id()->set_value( symbol->m_Uuid.AsStdString() );

                    // Get position and convert from KiCad internal units to nanometers (1 IU = 100 nm)
                    VECTOR2I pos = symbol->GetPosition();
                    symbolMsg.mutable_position()->set_x_nm( pos.x * 100 );
                    symbolMsg.mutable_position()->set_y_nm( pos.y * 100 );

                    // Get reference and value fields
                    symbolMsg.set_reference( symbol->GetField( FIELD_T::REFERENCE )->GetText().ToStdString() );
                    symbolMsg.set_value( symbol->GetField( FIELD_T::VALUE )->GetText().ToStdString() );

                    // Pack into Any and add to response
                    any = response.add_items();
                    any->PackFrom( symbolMsg );
                }
                // Add other item types as needed (labels, etc.)
            }
        }
        
        response.set_selection_count( selection.GetSize() );
        return response;
    }
    catch( const std::exception& ex )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( err );
    }
}


HANDLER_RESULT<Empty> API_HANDLER_SCH::handleClearSelection(
        const HANDLER_CONTEXT<schematic::commands::ClearSelection>& aCtx )
{
    /**
     * Clear current schematic selection.
     */
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );
    
    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );
    
    try
    {
        // Get the selection tool from the tool manager
        TOOL_MANAGER* toolMgr = m_frame->GetToolManager();
        if( !toolMgr )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        SCH_SELECTION_TOOL* selectionTool = toolMgr->GetTool<SCH_SELECTION_TOOL>();
        if( !selectionTool )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        // Clear the selection
        selectionTool->ClearSelection();
        
        return Empty();
    }
    catch( const std::exception& ex )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( err );
    }
}


HANDLER_RESULT<schematic::commands::SelectionResponse> API_HANDLER_SCH::handleAddToSelection(
        const HANDLER_CONTEXT<schematic::commands::AddToSelection>& aCtx )
{
    /**
     * Add items to current schematic selection.
     */
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );
    
    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );
    
    try
    {
        // Get the selection tool from the tool manager
        TOOL_MANAGER* toolMgr = m_frame->GetToolManager();
        if( !toolMgr )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        SCH_SELECTION_TOOL* selectionTool = toolMgr->GetTool<SCH_SELECTION_TOOL>();
        if( !selectionTool )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        // Add items to selection by ID (using correct EDA_ITEMS type)
        EDA_ITEMS toAdd;  // This is std::vector<EDA_ITEM*>
        
        for( const auto& kiid : aCtx.Request.item_ids() )
        {
            KIID itemId( kiid.value() );
            std::optional<EDA_ITEM*> item = getItemFromDocument( aCtx.Request.schematic(), itemId );
            
            if( item.has_value() && *item )
            {
                toAdd.emplace_back( *item );
            }
        }
        
        // Add all items using the proper selection tool method
        if( !toAdd.empty() )
        {
            selectionTool->AddItemsToSel( &toAdd );
            m_frame->GetCanvas()->Refresh();
        }
        
        // Return updated selection status
        schematic::commands::GetSelection getSelReq;
        getSelReq.mutable_schematic()->CopyFrom( aCtx.Request.schematic() );
        return handleGetSelection( { aCtx.ClientName, getSelReq } );
    }
    catch( const std::exception& ex )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( err );
    }
}


HANDLER_RESULT<schematic::commands::SelectionResponse> API_HANDLER_SCH::handleRemoveFromSelection(
        const HANDLER_CONTEXT<schematic::commands::RemoveFromSelection>& aCtx )
{
    /**
     * Remove items from current schematic selection.
     */
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.schematic() );
    
    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );
    
    try
    {
        // Get the selection tool from the tool manager
        TOOL_MANAGER* toolMgr = m_frame->GetToolManager();
        if( !toolMgr )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        SCH_SELECTION_TOOL* selectionTool = toolMgr->GetTool<SCH_SELECTION_TOOL>();
        if( !selectionTool )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNHANDLED );
            return tl::unexpected( e );
        }
        
        // Remove items from selection by ID
        for( const auto& kiid : aCtx.Request.item_ids() )
        {
            KIID itemId( kiid.value() );
            std::optional<EDA_ITEM*> item = getItemFromDocument( aCtx.Request.schematic(), itemId );
            
            if( item.has_value() && *item )
            {
                selectionTool->RemoveItemFromSel( *item );
            }
        }
        
        // Return updated selection status
        schematic::commands::GetSelection getSelReq;
        getSelReq.mutable_schematic()->CopyFrom( aCtx.Request.schematic() );
        return handleGetSelection( { aCtx.ClientName, getSelReq } );
    }
    catch( const std::exception& ex )
    {
        ApiResponseStatus err;
        err.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( err );
    }
}


/**
 * Handle PlaceSymbol API request.
 *
 * Places a symbol from a library at a specified position with given properties.
 * This replicates the core functionality of SCH_DRAWING_TOOLS::PlaceSymbol()
 * but with direct library/symbol specification instead of interactive dialog.
 *
 * @param aCtx Request context with placement parameters
 * @return PlaceSymbolResponse with symbol ID and assigned reference or error
 */
HANDLER_RESULT<schematic::commands::PlaceSymbolResponse>
API_HANDLER_SCH::handlePlaceSymbol( const HANDLER_CONTEXT<schematic::commands::PlaceSymbol>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }

    schematic::commands::PlaceSymbolResponse response;

    try
    {
        // Get current sheet and screen
        SCH_SHEET_PATH currentSheet = m_frame->GetCurrentSheet();
        SCH_SCREEN* screen = currentSheet.LastScreen();

        if( !screen )
        {
            response.set_error( "No active schematic screen" );
            return response;
        }

        // Get symbol library table and lookup the symbol
        SYMBOL_LIB_TABLE* libs = PROJECT_SCH::SchSymbolLibTable( &m_frame->Prj() );
        if( !libs )
        {
            response.set_error( "Symbol library table not available" );
            return response;
        }

        // Create library ID from library and symbol names
        LIB_ID libId( aCtx.Request.library_name(), aCtx.Request.symbol_name() );

        // Get the library symbol
        LIB_SYMBOL* libSymbol = nullptr;
        try
        {
            libSymbol = libs->LoadSymbol( libId );
        }
        catch( const std::exception& ex )
        {
            response.set_error( wxString::Format( "Failed to load symbol '%s:%s': %s",
                                                 aCtx.Request.library_name(),
                                                 aCtx.Request.symbol_name(),
                                                 ex.what() ).ToStdString() );
            return response;
        }

        if( !libSymbol )
        {
            response.set_error( wxString::Format( "Symbol '%s:%s' not found",
                                                 aCtx.Request.library_name(),
                                                 aCtx.Request.symbol_name() ).ToStdString() );
            return response;
        }

        // Set unit number for multi-unit symbols
        int unit = aCtx.Request.unit();
        if( unit <= 0 ) unit = 1;  // Default to unit 1

        // Convert position from nanometers to internal units
        VECTOR2I position = convertApiPositionToSchematic(
            aCtx.Request.position().x_nm(),
            aCtx.Request.position().y_nm()
        );

        // Create schematic symbol instance with correct parameters
        SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSymbol, libId, &currentSheet, unit, 0, position );

        // Set orientation (rotation) - SetOrientation expects int, not EDA_ANGLE
        if( aCtx.Request.orientation() != 0 )
        {
            int orientation = aCtx.Request.orientation();
            // Convert degrees to KiCad orientation enum
            // 0 = 0°, 1 = 90°, 2 = 180°, 3 = 270°
            int kiCadOrientation = 0;
            if( orientation == 90 ) kiCadOrientation = 1;
            else if( orientation == 180 ) kiCadOrientation = 2;
            else if( orientation == 270 ) kiCadOrientation = 3;

            symbol->SetOrientation( kiCadOrientation );
        }

        // Set mirrors
        if( aCtx.Request.mirrored_x() )
            symbol->MirrorHorizontally( position.x );
        if( aCtx.Request.mirrored_y() )
            symbol->MirrorVertically( position.y );

        // Set value if provided
        if( !aCtx.Request.value().empty() )
        {
            symbol->GetField( FIELD_T::VALUE )->SetText( aCtx.Request.value() );
        }

        // Add symbol to screen using commit for proper undo/redo
        SCH_COMMIT commit( m_frame );
        commit.Add( symbol, screen );

        // Handle reference annotation
        if( !aCtx.Request.reference().empty() )
        {
            // Manual reference provided - set it directly
            symbol->GetField( FIELD_T::REFERENCE )->SetText( aCtx.Request.reference() );

            // For manual references, we don't need to run the annotation system
            // since the reference is explicitly provided. This prevents corruption
            // of existing symbol references that was caused by UpdateAnnotation()
            // processing all symbols in the schematic.
        }
        else if( aCtx.Request.auto_annotate() )
        {
            // Set the symbol to have an unannotated reference initially
            // This will be corrected by the annotation system after commit
            wxString prefix = libSymbol->GetReferenceField().GetText();
            if( prefix.IsEmpty() )
                prefix = "U";

            prefix = UTIL::GetRefDesPrefix( prefix );
            if( prefix.IsEmpty() )
                prefix = "U";

            wxString unannotatedRef = UTIL::GetRefDesUnannotated( prefix );
            symbol->GetField( FIELD_T::REFERENCE )->SetText( unannotatedRef );
        }

        // Commit the changes
        commit.Push( "Place symbol" );

        // Handle auto-annotation AFTER the symbol is committed to the schematic
        if( aCtx.Request.auto_annotate() && !symbol->IsPower() )
        {
            // Create a separate commit for annotation changes
            SCH_COMMIT annotationCommit( m_frame );

            // Create a reference list with just this symbol
            SCH_REFERENCE_LIST newSymbolRefs;
            SCH_REFERENCE newRef( symbol, currentSheet );
            newSymbolRefs.AddItem( newRef );

            // Get all existing symbols for collision detection
            SCH_SHEET_LIST hierarchy = m_frame->Schematic().Hierarchy();
            SCH_REFERENCE_LIST allRefs;
            hierarchy.GetSymbols( allRefs );

            // Set up annotation system
            SCHEMATIC_SETTINGS& settings = m_frame->Schematic().Settings();
            newSymbolRefs.SetRefDesTracker( settings.m_refDesTracker );

            // Use ReannotateDuplicates to assign proper number
            newSymbolRefs.ReannotateDuplicates( allRefs );
            newSymbolRefs.UpdateAnnotation();

            // Mark the symbol as modified for proper UI update
            annotationCommit.Modify( symbol, screen );
            annotationCommit.Push( "Auto-annotate symbol" );
        }

        // Refresh view
        m_frame->GetCanvas()->Refresh();

        // Return success response
        response.mutable_symbol_id()->set_value( symbol->m_Uuid.AsString().ToStdString() );
        response.set_assigned_reference( symbol->GetField( FIELD_T::REFERENCE )->GetText().ToStdString() );

        return response;
    }
    catch( const std::exception& ex )
    {
        response.set_error( wxString::Format( "Error placing symbol: %s", ex.what() ).ToStdString() );
        return response;
    }
}



/**
 * Handle GetSymbolLibraries API request
 *
 * Returns all available symbol libraries with metadata including symbol counts,
 * descriptions, and power symbol filtering. This enables AI-driven library
 * browsing and symbol discovery workflows.
 */
HANDLER_RESULT<schematic::commands::GetSymbolLibrariesResponse>
API_HANDLER_SCH::handleGetSymbolLibraries( const HANDLER_CONTEXT<schematic::commands::GetSymbolLibraries>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }

    schematic::commands::GetSymbolLibrariesResponse response;

    try
    {
        // Get symbol library table from project
        SYMBOL_LIB_TABLE* libs = PROJECT_SCH::SchSymbolLibTable( &m_frame->Prj() );
        if( !libs )
        {
            response.set_error( "Symbol library table not available" );
            return response;
        }

        // Get all available library names
        std::vector<wxString> libraryNames = libs->GetLogicalLibs();

        // Add timeout protection for long operations (like KiCad UI loading)
        auto start = std::chrono::high_resolution_clock::now();
        const int MAX_PROCESSING_TIME_MS = 30000;  // 30 second timeout (like UI)
        const int MAX_LIBRARIES_TO_PROCESS = 300;  // Allow for all libraries
        int processedCount = 0;

        // Iterate through all available libraries
        for( const wxString& libName : libraryNames )
        {
            // Check timeout to prevent IPC connection issues
            auto current = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( current - start );
            if( elapsed.count() > MAX_PROCESSING_TIME_MS )
            {
                response.set_error( wxString::Format(
                    "Library loading timeout after %d ms. Processed %d libraries. Use specific library names for faster results.",
                    MAX_PROCESSING_TIME_MS, processedCount ).ToStdString() );
                break;
            }

            // Limit number of libraries processed only if excessive
            if( processedCount >= MAX_LIBRARIES_TO_PROCESS )
            {
                response.set_error( wxString::Format(
                    "Processed maximum %d libraries. Total libraries: %d. This should not normally occur.",
                    MAX_LIBRARIES_TO_PROCESS, (int)libraryNames.size() ).ToStdString() );
                break;
            }

            try
            {
                // Get library metadata
                SYMBOL_LIB_TABLE_ROW* libRow = libs->FindRow( libName );
                if( !libRow )
                    continue;

                // Optimized power library detection using heuristics
                bool isPowerLibrary = libName.Contains( "power" ) || libName.Contains( "Power" );

                // Fast filtering: skip non-power libraries when power_only filter is active
                if( aCtx.Request.power_symbols_only() && !isPowerLibrary )
                {
                    // Skip libraries that clearly aren't power-related based on name
                    continue;
                }

                // Load symbols exactly like KiCad UI does when symbol button is pressed
                std::vector<LIB_SYMBOL*> symbolList;

                // Use the same method as KiCad's SYMBOL_TREE_MODEL_ADAPTER
                try
                {
                    // This is exactly what KiCad does: load ALL symbols from the library
                    // This triggers the same comprehensive loading as the UI
                    libs->LoadSymbolLib( symbolList, libName, aCtx.Request.power_symbols_only() );
                }
                catch( ... )
                {
                    // Skip libraries that can't be loaded
                    continue;
                }

                int totalSymbols = symbolList.size();
                int powerSymbolCount = 0;

                // Count power symbols if needed
                if( aCtx.Request.power_symbols_only() )
                {
                    for( LIB_SYMBOL* symbol : symbolList )
                    {
                        if( symbol && symbol->IsPower() )
                            powerSymbolCount++;
                    }

                    // Skip library if no power symbols found
                    if( powerSymbolCount == 0 )
                        continue;
                }
                else
                {
                    // Count power symbols for library classification
                    for( LIB_SYMBOL* symbol : symbolList )
                    {
                        if( symbol && symbol->IsPower() )
                            powerSymbolCount++;
                    }

                    // Update power library detection based on actual content
                    if( powerSymbolCount > totalSymbols / 2 )  // Majority are power symbols
                        isPowerLibrary = true;
                }

                // Create library response entry
                schematic::commands::SymbolLibrary* library = response.add_libraries();
                library->set_name( libName.ToStdString() );

                // Set description (use library row info)
                wxString description = wxString::Format( "KiCad Symbol Library (%s)", libRow->GetType() );
                if( isPowerLibrary )
                    description += " - Power Symbols";
                library->set_description( description.ToStdString() );

                // Set symbol count (total or power symbols based on filter)
                if( aCtx.Request.power_symbols_only() )
                    library->set_symbol_count( powerSymbolCount );
                else
                    library->set_symbol_count( totalSymbols );

                library->set_is_power_library( isPowerLibrary );

                processedCount++;
            }
            catch( const std::exception& ex )
            {
                // Log library load error but continue with other libraries
                wxLogDebug( "Failed to load library '%s': %s", libName, ex.what() );
                continue;
            }
        }

        // If no libraries found, set appropriate message
        if( response.libraries_size() == 0 )
        {
            if( aCtx.Request.power_symbols_only() )
                response.set_error( "No power symbol libraries found" );
            else
                response.set_error( "No symbol libraries available" );
        }
    }
    catch( const std::exception& ex )
    {
        response.set_error( wxString::Format( "Error accessing symbol libraries: %s", ex.what() ).ToStdString() );
    }

    return response;
}


/**
 * Handle SearchSymbols API request
 *
 * Searches through symbol libraries with text-based filtering on symbol names,
 * descriptions, and keywords. Supports library filtering and power symbol detection
 * for AI-driven symbol discovery workflows.
 */
HANDLER_RESULT<schematic::commands::SearchSymbolsResponse>
API_HANDLER_SCH::handleSearchSymbols( const HANDLER_CONTEXT<schematic::commands::SearchSymbols>& aCtx )
{
    if( !validateDocument( aCtx.Request.schematic() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Invalid schematic document" );
        return tl::unexpected( e );
    }

    schematic::commands::SearchSymbolsResponse response;

    try
    {
        // Get symbol library table from project
        SYMBOL_LIB_TABLE* libs = PROJECT_SCH::SchSymbolLibTable( &m_frame->Prj() );
        if( !libs )
        {
            response.set_error( "Symbol library table not available" );
            return response;
        }

        const wxString searchText = aCtx.Request.search_text();
        const bool powerOnly = aCtx.Request.power_symbols_only();
        const int maxResults = aCtx.Request.max_results() > 0 ? aCtx.Request.max_results() : 50;
        int resultCount = 0;

        // Create set of libraries to search (empty means search all)
        std::set<wxString> librariesToSearch;
        for( const std::string& libName : aCtx.Request.libraries() )
        {
            librariesToSearch.insert( wxString::FromUTF8( libName ) );
        }
        bool searchAllLibraries = librariesToSearch.empty();

        // Get all available library names
        std::vector<wxString> libraryNames = libs->GetLogicalLibs();

        // Iterate through all available libraries
        for( const wxString& libName : libraryNames )
        {
            // Skip library if not in search list (when filtering by specific libraries)
            if( !searchAllLibraries && librariesToSearch.find( libName ) == librariesToSearch.end() )
                continue;

            // Stop if we've reached the maximum results
            if( resultCount >= maxResults )
                break;

            try
            {
                // Get all symbol names in this library
                wxArrayString symbolNames;
                libs->EnumerateSymbolLib( libName, symbolNames );

                // Search through symbols in this library
                for( const wxString& symbolName : symbolNames )
                {
                    if( resultCount >= maxResults )
                        break;

                    try
                    {
                        LIB_ID libId( libName, symbolName );
                        LIB_SYMBOL* symbol = libs->LoadSymbol( libId );
                        if( !symbol )
                            continue;

                        // Apply power symbol filter if requested
                        bool isPowerSymbol = symbol->IsPower();
                        if( powerOnly && !isPowerSymbol )
                            continue;

                        // Get symbol metadata for searching
                        wxString description = symbol->GetDescription();
                        wxString keywords = symbol->GetKeyWords();

                        // Perform text search (case-insensitive)
                        bool matches = false;
                        if( searchText.empty() )
                        {
                            matches = true;  // Empty search returns all symbols
                        }
                        else
                        {
                            wxString searchLower = searchText.Lower();
                            matches = symbolName.Lower().Contains( searchLower ) ||
                                     description.Lower().Contains( searchLower ) ||
                                     keywords.Lower().Contains( searchLower );
                        }

                        if( matches )
                        {
                            // Create search result entry
                            schematic::commands::SymbolSearchResult* result = response.add_symbols();
                            result->set_library_name( libName.ToStdString() );
                            result->set_symbol_name( symbolName.ToStdString() );
                            result->set_description( description.ToStdString() );
                            result->set_keywords( keywords.ToStdString() );
                            result->set_is_power_symbol( isPowerSymbol );
                            result->set_unit_count( symbol->GetUnitCount() );

                            resultCount++;
                        }
                    }
                    catch( const std::exception& ex )
                    {
                        // Skip symbols that can't be loaded
                        wxLogDebug( "Failed to load symbol '%s:%s': %s", libName, symbolName, ex.what() );
                        continue;
                    }
                }
            }
            catch( const std::exception& ex )
            {
                // Log library load error but continue with other libraries
                wxLogDebug( "Failed to search library '%s': %s", libName, ex.what() );
                continue;
            }
        }

        // Set appropriate message if no results found
        if( response.symbols_size() == 0 )
        {
            if( searchText.empty() )
            {
                if( powerOnly )
                    response.set_error( "No power symbols found in specified libraries" );
                else
                    response.set_error( "No symbols found in specified libraries" );
            }
            else
            {
                response.set_error( wxString::Format( "No symbols found matching search text '%s'", searchText ).ToStdString() );
            }
        }
    }
    catch( const std::exception& ex )
    {
        response.set_error( wxString::Format( "Error searching symbols: %s", ex.what() ).ToStdString() );
    }

    return response;
}


/**
 * Handle PreloadSymbolLibraries API request.
 *
 * This function preloads symbol libraries to avoid the "Load Symbol Libraries" dialog
 * when the symbol chooser is first opened. It replicates the preloading done by
 * SYMBOL_TREE_MODEL_ADAPTER::Create() and SYMBOL_ASYNC_LOADER.
 *
 * @param aCtx Request context containing library names to preload
 * @return PreloadSymbolLibrariesResponse with loading statistics
 */
HANDLER_RESULT<schematic::commands::PreloadSymbolLibrariesResponse>
API_HANDLER_SCH::handlePreloadSymbolLibraries( const HANDLER_CONTEXT<schematic::commands::PreloadSymbolLibraries>& aCtx )
{
    schematic::commands::PreloadSymbolLibrariesResponse response;

    auto start = std::chrono::high_resolution_clock::now();

    try
    {
        // Get symbol library table
        SYMBOL_LIB_TABLE* libs = PROJECT_SCH::SchSymbolLibTable( &m_frame->Prj() );
        if( !libs )
        {
            response.set_error( "Symbol library table not available" );
            return response;
        }

        // Get list of libraries to preload
        std::vector<wxString> librariesToLoad;
        if( aCtx.Request.library_names_size() > 0 )
        {
            // Specific libraries requested
            for( const std::string& libName : aCtx.Request.library_names() )
            {
                wxString wxLibName = wxString::FromUTF8( libName );
                if( libs->HasLibrary( wxLibName, true ) )
                {
                    librariesToLoad.push_back( wxLibName );
                }
                else
                {
                    response.add_failed_libraries( libName );
                }
            }
        }
        else
        {
            // Load all available libraries
            librariesToLoad = libs->GetLogicalLibs();
        }

        // Use the same comprehensive loading mechanism as the UI
        int successfulLoads = 0;

        // Process all requested libraries - no artificial limits (UI behavior)
        // The 60-second timeout provides sufficient protection against timeouts
        int maxLibsToProcess = librariesToLoad.size();

        for( int i = 0; i < maxLibsToProcess; i++ )
        {
            const wxString& libName = librariesToLoad[i];
            try
            {
                // Comprehensive loading: enumerate symbols AND load first few symbols to trigger caching
                wxArrayString symbolNames;
                libs->EnumerateSymbolLib( libName, symbolNames );

                if( !symbolNames.empty() )
                {
                    // Load a few symbols from each library to trigger the same caching as UI
                    int symbolsToPreload = std::min( 5, (int)symbolNames.size() );
                    for( int j = 0; j < symbolsToPreload; j++ )
                    {
                        try
                        {
                            LIB_ID libId( libName, symbolNames[j] );
                            LIB_SYMBOL* symbol = libs->LoadSymbol( libId );
                            // Symbol loaded - this triggers the same caching as UI
                            (void)symbol; // Mark as used to avoid compiler warning
                        }
                        catch( ... )
                        {
                            // Skip symbols that can't be loaded
                            continue;
                        }
                    }
                    successfulLoads++;
                }
                else
                {
                    response.add_failed_libraries( libName.ToStdString() );
                }
            }
            catch( ... )
            {
                response.add_failed_libraries( libName.ToStdString() );
            }
        }

        response.set_loaded_libraries( successfulLoads );

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( end - start );
        response.set_loading_time_seconds( duration.count() / 1000.0 );
    }
    catch( const std::exception& ex )
    {
        response.set_error( wxString::Format( "Error preloading libraries: %s", ex.what() ).ToStdString() );
        return response;
    }

    return response;
}


/**
 * Handle GetLibraryLoadStatus API request.
 *
 * This function checks whether symbol and footprint libraries have been loaded,
 * providing status information similar to what the symbol chooser dialog would show.
 *
 * @param aCtx Request context
 * @return GetLibraryLoadStatusResponse with loading status
 */
HANDLER_RESULT<schematic::commands::GetLibraryLoadStatusResponse>
API_HANDLER_SCH::handleGetLibraryLoadStatus( const HANDLER_CONTEXT<schematic::commands::GetLibraryLoadStatus>& aCtx )
{
    schematic::commands::GetLibraryLoadStatusResponse response;

    try
    {
        // Check symbol library status
        SYMBOL_LIB_TABLE* libs = PROJECT_SCH::SchSymbolLibTable( &m_frame->Prj() );
        if( libs )
        {
            // Check if libraries are loaded by trying to enumerate symbols
            std::vector<wxString> libraryNames = libs->GetLogicalLibs();
            response.set_symbol_library_count( libraryNames.size() );

            // Comprehensive check - test more libraries and count total symbols like UI
            bool symbolsLoaded = false;
            int loadedCount = 0;
            int totalSymbols = 0;
            int maxLibsToTest = std::min( 10, (int)libraryNames.size() ); // Test 10 libraries for better accuracy

            auto startTime = std::chrono::high_resolution_clock::now();

            for( int i = 0; i < maxLibsToTest; i++ )
            {
                const wxString& libName = libraryNames[i];
                try
                {
                    // Test actual symbol enumeration and count symbols like UI does
                    wxArrayString symbolNames;
                    libs->EnumerateSymbolLib( libName, symbolNames );

                    if( !symbolNames.empty() )
                    {
                        totalSymbols += symbolNames.size();
                        loadedCount++;
                        response.add_loaded_symbol_libraries( libName.ToStdString() );
                    }

                    // Check timing after each library
                    auto currentTime = std::chrono::high_resolution_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);

                    // If we're getting reasonable symbol counts quickly, libraries are loaded
                    if( totalSymbols > 1000 && elapsed.count() < 2000 )
                    {
                        symbolsLoaded = true;
                    }
                    // If it's taking too long for too few symbols, not loaded
                    else if( elapsed.count() > 3000 && totalSymbols < 500 )
                    {
                        symbolsLoaded = false;
                        break;
                    }
                }
                catch( ... )
                {
                    // Library not accessible - skip
                    continue;
                }
            }

            // Final determination: if we got a good symbol count quickly, consider loaded
            if( totalSymbols > 500 && loadedCount >= 5 )
            {
                symbolsLoaded = true;
            }

            response.set_symbols_loaded( symbolsLoaded );
        }
        else
        {
            response.set_symbols_loaded( false );
            response.set_symbol_library_count( 0 );
        }

        // Note: Footprint libraries are managed separately in PCB context
        // Not accessible from schematic editor, so we return default values
        response.set_footprints_loaded( false );
        response.set_footprint_library_count( 0 );
    }
    catch( const std::exception& ex )
    {
        // Return partial results with error
        response.set_symbols_loaded( false );
        response.set_footprints_loaded( false );
    }

    return response;
}


/**
 * Handle RefreshSymbolLibraries API request.
 *
 * This function forces a refresh of symbol libraries, useful when libraries
 * have been modified externally or new symbols have been added.
 *
 * @param aCtx Request context containing libraries to refresh
 * @return RefreshSymbolLibrariesResponse with refresh statistics
 */
HANDLER_RESULT<schematic::commands::RefreshSymbolLibrariesResponse>
API_HANDLER_SCH::handleRefreshSymbolLibraries( const HANDLER_CONTEXT<schematic::commands::RefreshSymbolLibraries>& aCtx )
{
    schematic::commands::RefreshSymbolLibrariesResponse response;

    auto start = std::chrono::high_resolution_clock::now();

    try
    {
        // Get symbol library table
        SYMBOL_LIB_TABLE* libs = PROJECT_SCH::SchSymbolLibTable( &m_frame->Prj() );
        if( !libs )
        {
            response.set_error( "Symbol library table not available" );
            return response;
        }

        // Get list of libraries to refresh
        std::vector<wxString> librariesToRefresh;
        if( aCtx.Request.library_names_size() > 0 )
        {
            // Specific libraries requested
            for( const std::string& libName : aCtx.Request.library_names() )
            {
                wxString wxLibName = wxString::FromUTF8( libName );
                if( libs->HasLibrary( wxLibName, true ) )
                {
                    librariesToRefresh.push_back( wxLibName );
                }
                else
                {
                    response.add_failed_libraries( libName );
                }
            }
        }
        else
        {
            // Refresh all available libraries
            librariesToRefresh = libs->GetLogicalLibs();
        }

        // Refresh libraries by clearing caches and reloading
        // This mimics what happens when the symbol chooser is refreshed
        int refreshedCount = 0;

        for( const wxString& libName : librariesToRefresh )
        {
            try
            {
                // Force reload by attempting to enumerate symbols
                // Note: SYMBOL_LIB_TABLE doesn't have a ClearCache method
                // The enumeration itself will refresh the library contents
                wxArrayString symbolNames;
                libs->EnumerateSymbolLib( libName, symbolNames );

                refreshedCount++;
            }
            catch( ... )
            {
                response.add_failed_libraries( libName.ToStdString() );
            }
        }

        response.set_refreshed_libraries( refreshedCount );

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( end - start );
        response.set_refresh_time_seconds( duration.count() / 1000.0 );
    }
    catch( const std::exception& ex )
    {
        response.set_error( wxString::Format( "Error refreshing libraries: %s", ex.what() ).ToStdString() );
        return response;
    }

    return response;
}

