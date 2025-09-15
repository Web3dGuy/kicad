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
#include <wx/filename.h>
#include <tool/tool_manager.h>
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
