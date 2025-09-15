/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#include <api/api_sch_validation.h>
#include <api/common/commands/editor_commands.pb.h>
#include <fmt/format.h>
#include <wx/string.h>
#include <cmath>
#include <algorithm>

using namespace API_SCH_VALIDATION;

// ============================================================================
// CoordinateValidator Implementation
// ============================================================================

std::optional<ValidationError> CoordinateValidator::ValidateCoordinate( int64_t aValue,
                                                                        const std::string& aFieldName )
{
    if( aValue < MIN_COORD_NM || aValue > MAX_COORD_NM )
    {
        double valueMM = static_cast<double>( aValue ) / 1000000.0;
        ValidationError error(
            fmt::format( "{} coordinate {:.1f}mm ({}nm) exceeds valid range ({:.0f}mm to {:.0f}mm)",
                        aFieldName, valueMM, aValue, PRACTICAL_MIN_MM, PRACTICAL_MAX_MM ),
            aFieldName,
            std::to_string( aValue )
        );

        error.AddSuggestion( fmt::format( "Use coordinates between {:.0f}mm and {:.0f}mm",
                                         PRACTICAL_MIN_MM, PRACTICAL_MAX_MM ) );
        error.AddSuggestion( "Convert coordinates to nanometers (1mm = 1,000,000nm)" );
        error.AddSuggestion( "Check if coordinates are in correct units (should be nanometers, not micrometers)" );

        return error;
    }

    return std::nullopt;
}

std::optional<ValidationError> CoordinateValidator::ValidatePosition( const VECTOR2I& aPosition,
                                                                     const std::string& aFieldName )
{
    // Validate X coordinate
    auto xResult = ValidateCoordinate( aPosition.x, aFieldName + ".x" );
    if( xResult )
        return xResult;

    // Validate Y coordinate
    auto yResult = ValidateCoordinate( aPosition.y, aFieldName + ".y" );
    if( yResult )
        return yResult;

    return std::nullopt;
}

// ============================================================================
// WireValidator Implementation
// ============================================================================

std::optional<ValidationError> WireValidator::ValidateWireGeometry( const VECTOR2I& aStartPos,
                                                                   const VECTOR2I& aEndPos,
                                                                   int aWireWidth )
{
    // Validate start position
    auto startResult = CoordinateValidator::ValidatePosition( aStartPos, "start_point" );
    if( startResult )
        return startResult;

    // Validate end position
    auto endResult = CoordinateValidator::ValidatePosition( aEndPos, "end_point" );
    if( endResult )
        return endResult;

    // Check for zero-length wire (identical start/end points)
    if( aStartPos == aEndPos )
    {
        ValidationError error(
            "Wire start and end points are identical (zero-length wire)",
            "wire_geometry",
            fmt::format( "start({},{}), end({},{})", aStartPos.x, aStartPos.y, aEndPos.x, aEndPos.y )
        );

        error.AddSuggestion( "Ensure start_point and end_point are different" );
        error.AddSuggestion( "Check if coordinates are intended to create a visible wire" );
        error.AddSuggestion( fmt::format( "Minimum recommended wire length: {:.1f}mm",
                                         static_cast<double>( MIN_WIRE_LENGTH_NM ) / 1000000.0 ) );

        return error;
    }

    // Calculate wire length
    VECTOR2I delta = aEndPos - aStartPos;
    double lengthNM = std::sqrt( static_cast<double>( delta.x ) * delta.x +
                                static_cast<double>( delta.y ) * delta.y );
    double lengthMM = lengthNM / 1000000.0;

    // Check minimum wire length (warn for very short wires)
    if( lengthNM < MIN_WIRE_LENGTH_NM )
    {
        ValidationError error(
            fmt::format( "Wire length {:.3f}mm is very short and may not be visible", lengthMM ),
            "wire_length",
            fmt::format( "{:.3f}", lengthMM )
        );

        error.AddSuggestion( fmt::format( "Recommended minimum wire length: {:.1f}mm",
                                         static_cast<double>( MIN_WIRE_LENGTH_NM ) / 1000000.0 ) );
        error.AddSuggestion( "Verify coordinates are correct and in nanometers" );
        error.AddSuggestion( "Consider if this wire segment is necessary" );

        return error;
    }

    // Validate wire width if specified
    if( aWireWidth != 0 )
    {
        auto widthResult = ValidateWireWidth( aWireWidth );
        if( widthResult )
            return widthResult;
    }

    return std::nullopt;
}

std::optional<ValidationError> WireValidator::ValidateWireWidth( int aWidth )
{
    if( aWidth < MIN_WIRE_WIDTH_NM )
    {
        ValidationError error(
            fmt::format( "Wire width {}nm cannot be negative", aWidth ),
            "width",
            std::to_string( aWidth )
        );

        error.AddSuggestion( "Use 0 for default width" );
        error.AddSuggestion( "Specify positive width in nanometers (e.g., 150000 for 0.15mm)" );

        return error;
    }

    if( aWidth > MAX_WIRE_WIDTH_NM )
    {
        double widthMM = static_cast<double>( aWidth ) / 1000000.0;
        double maxMM = static_cast<double>( MAX_WIRE_WIDTH_NM ) / 1000000.0;

        ValidationError error(
            fmt::format( "Wire width {:.1f}mm exceeds maximum {:.1f}mm", widthMM, maxMM ),
            "width",
            std::to_string( aWidth )
        );

        error.AddSuggestion( fmt::format( "Use width between 0 and {:.1f}mm", maxMM ) );
        error.AddSuggestion( "Check if width is in correct units (should be nanometers)" );

        return error;
    }

    return std::nullopt;
}

// ============================================================================
// TextValidator Implementation
// ============================================================================

std::optional<ValidationError> TextValidator::ValidateTextContent( const std::string& aText,
                                                                  const std::string& aFieldName,
                                                                  bool aAllowEmpty )
{
    // Trim whitespace for length check
    std::string trimmedText = aText;
    trimmedText.erase( std::remove_if( trimmedText.begin(), trimmedText.end(), ::isspace ),
                      trimmedText.end() );

    // Check for empty text
    if( !aAllowEmpty && trimmedText.empty() )
    {
        ValidationError error(
            fmt::format( "{} cannot be empty", aFieldName ),
            aFieldName,
            aText
        );

        error.AddSuggestion( "Provide meaningful label text (e.g., 'VCC', 'GND', 'RESET')" );
        error.AddSuggestion( "Use descriptive names for net labels" );
        error.AddSuggestion( "Remove label if no text is needed" );

        return error;
    }

    // Check text length
    if( aText.length() > MAX_TEXT_LENGTH )
    {
        ValidationError error(
            fmt::format( "{} length {} exceeds maximum {}", aFieldName, aText.length(), MAX_TEXT_LENGTH ),
            aFieldName,
            std::to_string( aText.length() )
        );

        error.AddSuggestion( fmt::format( "Limit text to {} characters", MAX_TEXT_LENGTH ) );
        error.AddSuggestion( "Use shorter, more concise label names" );

        return error;
    }

    return std::nullopt;
}

// ============================================================================
// ItemValidator Implementation
// ============================================================================

std::optional<ValidationError> ItemValidator::ValidateWireItem( const SCH_LINE* aItem )
{
    if( !aItem )
    {
        return ValidationError( "Wire item is null", "wire_item", "null" );
    }

    // Only validate electrical wires, not graphical lines
    if( !aItem->IsWire() )
        return std::nullopt;

    VECTOR2I startPos = aItem->GetStartPoint();
    VECTOR2I endPos = aItem->GetEndPoint();
    int width = aItem->GetLineWidth();

    return WireValidator::ValidateWireGeometry( startPos, endPos, width );
}

std::optional<ValidationError> ItemValidator::ValidateLabelItem( const SCH_LABEL_BASE* aItem )
{
    if( !aItem )
    {
        return ValidationError( "Label item is null", "label_item", "null" );
    }

    // Validate position
    VECTOR2I position = aItem->GetPosition();
    auto posResult = CoordinateValidator::ValidatePosition( position, "label_position" );
    if( posResult )
        return posResult;

    // Validate text content (allow empty for some label types)
    std::string text = aItem->GetText().ToStdString();
    bool allowEmpty = ( aItem->Type() == SCH_HIER_LABEL_T );  // Hierarchical labels can be empty

    auto textResult = TextValidator::ValidateTextContent( text, "label_text", allowEmpty );
    if( textResult )
        return textResult;

    return std::nullopt;
}

std::optional<ValidationError> ItemValidator::ValidateJunctionItem( const SCH_JUNCTION* aItem )
{
    if( !aItem )
    {
        return ValidationError( "Junction item is null", "junction_item", "null" );
    }

    // Validate position
    VECTOR2I position = aItem->GetPosition();
    return CoordinateValidator::ValidatePosition( position, "junction_position" );
}

std::optional<ValidationError> ItemValidator::ValidateSymbolItem( const SCH_SYMBOL* aItem )
{
    if( !aItem )
    {
        return ValidationError( "Symbol item is null", "symbol_item", "null" );
    }

    // Validate position
    VECTOR2I position = aItem->GetPosition();
    auto posResult = CoordinateValidator::ValidatePosition( position, "symbol_position" );
    if( posResult )
        return posResult;

    // Validate reference (if set)
    wxString reference = aItem->GetRef( nullptr );
    if( !reference.IsEmpty() )
    {
        auto refResult = TextValidator::ValidateTextContent( reference.ToStdString(), "symbol_reference", false );
        if( refResult )
            return refResult;
    }

    // Validate value (if set)
    wxString value = aItem->GetValue( false, nullptr, false );
    if( !value.IsEmpty() )
    {
        auto valueResult = TextValidator::ValidateTextContent( value.ToStdString(), "symbol_value", true );
        if( valueResult )
            return valueResult;
    }

    return std::nullopt;
}

std::optional<ValidationError> ItemValidator::ValidateSchematicItem( const SCH_ITEM* aItem )
{
    if( !aItem )
    {
        return ValidationError( "Schematic item is null", "item", "null" );
    }

    // Type-specific validation
    switch( aItem->Type() )
    {
    case SCH_LINE_T:
        return ValidateWireItem( static_cast<const SCH_LINE*>( aItem ) );

    case SCH_LABEL_T:
    case SCH_GLOBAL_LABEL_T:
    case SCH_HIER_LABEL_T:
    case SCH_DIRECTIVE_LABEL_T:
        return ValidateLabelItem( static_cast<const SCH_LABEL_BASE*>( aItem ) );

    case SCH_JUNCTION_T:
        return ValidateJunctionItem( static_cast<const SCH_JUNCTION*>( aItem ) );

    case SCH_SYMBOL_T:
        return ValidateSymbolItem( static_cast<const SCH_SYMBOL*>( aItem ) );

    default:
        // For other item types, just validate basic positioning
        // All SCH_ITEM objects have GetPosition(), so we can always validate position
        VECTOR2I position = aItem->GetPosition();
        return CoordinateValidator::ValidatePosition( position, "item_position" );
    }

    return std::nullopt;
}

// ============================================================================
// ErrorFormatter Implementation
// ============================================================================

std::string ErrorFormatter::FormatValidationError( const ValidationError& aError )
{
    std::string formatted = aError.message;

    if( !aError.field.empty() )
        formatted += fmt::format( " (Field: {})", aError.field );

    if( !aError.value.empty() )
        formatted += fmt::format( " (Value: {})", aError.value );

    if( !aError.suggestions.empty() )
    {
        formatted += " Suggestions: ";
        for( size_t i = 0; i < aError.suggestions.size(); ++i )
        {
            if( i > 0 )
                formatted += "; ";
            formatted += aError.suggestions[i];
        }
    }

    return formatted;
}

void ErrorFormatter::ValidationErrorToItemStatus( const ValidationError& aError,
                                                  kiapi::common::commands::ItemStatus& aStatus )
{
    aStatus.set_code( kiapi::common::commands::ItemStatusCode::ISC_INVALID_DATA );
    aStatus.set_error_message( FormatValidationError( aError ) );
}