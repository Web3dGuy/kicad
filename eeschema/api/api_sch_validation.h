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

#ifndef API_SCH_VALIDATION_H
#define API_SCH_VALIDATION_H

#include <api/common/commands/editor_commands.pb.h>
#include <math/vector2d.h>
#include <sch_item.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_symbol.h>
#include <optional>
#include <string>

/**
 * Validation utilities for KiCad schematic API operations.
 *
 * This provides comprehensive validation to prevent silent data corruption
 * and improve error handling at the C++ API layer. Mirrors the validation
 * implemented in the MCP Python layer for consistent behavior.
 *
 * Based on comprehensive error analysis from Section 5 of the schematic API
 * optimization task, this addresses:
 * - Coordinate clamping issues (extreme coordinates silently clamped)
 * - Zero-length wire acceptance
 * - Negative wire width acceptance
 * - Empty label text handling
 * - Context-dependent error messages
 */

namespace API_SCH_VALIDATION
{
    /**
     * Custom validation error structure with helpful context
     */
    struct ValidationError
    {
        std::string message;
        std::string field;
        std::string value;
        std::vector<std::string> suggestions;

        ValidationError( const std::string& aMessage, const std::string& aField = "",
                        const std::string& aValue = "" )
            : message( aMessage ), field( aField ), value( aValue ) {}

        void AddSuggestion( const std::string& aSuggestion )
        {
            suggestions.push_back( aSuggestion );
        }
    };

    /**
     * Coordinate validation constants and functions
     */
    namespace CoordinateValidator
    {
        // KiCad coordinate limits (conservative bounds based on error analysis)
        // These prevent the silent clamping observed in testing
        constexpr int64_t MAX_COORD_NM = 100000000000000LL;  // 100m in nanometers
        constexpr int64_t MIN_COORD_NM = -100000000000000LL;

        // Practical schematic bounds (more realistic for error messages)
        constexpr double PRACTICAL_MAX_MM = 1000.0;  // 1m schematic sheet
        constexpr double PRACTICAL_MIN_MM = -1000.0;

        /**
         * Validate a single coordinate value
         * @param aValue Coordinate value in nanometers
         * @param aFieldName Name of the field for error reporting
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateCoordinate( int64_t aValue,
                                                          const std::string& aFieldName );

        /**
         * Validate a position (x,y coordinate pair)
         * @param aPosition Position vector in nanometers
         * @param aFieldName Name of the field for error reporting
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidatePosition( const VECTOR2I& aPosition,
                                                        const std::string& aFieldName );
    }

    /**
     * Wire geometry validation functions
     */
    namespace WireValidator
    {
        constexpr int64_t MIN_WIRE_LENGTH_NM = 1000000;   // 1mm minimum wire length
        constexpr int64_t MAX_WIRE_WIDTH_NM = 10000000;   // 10mm maximum wire width
        constexpr int64_t MIN_WIRE_WIDTH_NM = 0;          // 0 = use default width

        /**
         * Validate wire geometry and calculate properties
         * @param aStartPos Starting position
         * @param aEndPos Ending position
         * @param aWireWidth Wire width in nanometers
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateWireGeometry( const VECTOR2I& aStartPos,
                                                            const VECTOR2I& aEndPos,
                                                            int aWireWidth = 0 );

        /**
         * Validate wire width parameter
         * @param aWidth Wire width in nanometers
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateWireWidth( int aWidth );
    }

    /**
     * Text content validation functions
     */
    namespace TextValidator
    {
        constexpr size_t MAX_TEXT_LENGTH = 1000;  // Maximum text length
        constexpr size_t MIN_TEXT_LENGTH = 1;     // Minimum text length

        /**
         * Validate text content for labels and annotations
         * @param aText Text content to validate
         * @param aFieldName Name of the field for error reporting
         * @param aAllowEmpty Whether to allow empty text
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateTextContent( const std::string& aText,
                                                           const std::string& aFieldName,
                                                           bool aAllowEmpty = false );
    }

    /**
     * Item-specific validation functions
     */
    namespace ItemValidator
    {
        /**
         * Comprehensive validation for wire/line items
         * @param aItem SCH_LINE item to validate
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateWireItem( const SCH_LINE* aItem );

        /**
         * Comprehensive validation for label items
         * @param aItem SCH_LABEL_BASE item to validate
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateLabelItem( const SCH_LABEL_BASE* aItem );

        /**
         * Comprehensive validation for junction items
         * @param aItem SCH_JUNCTION item to validate
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateJunctionItem( const SCH_JUNCTION* aItem );

        /**
         * Comprehensive validation for symbol items
         * @param aItem SCH_SYMBOL item to validate
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateSymbolItem( const SCH_SYMBOL* aItem );

        /**
         * Generic validation for any schematic item
         * @param aItem SCH_ITEM to validate
         * @return std::nullopt if valid, ValidationError if invalid
         */
        std::optional<ValidationError> ValidateSchematicItem( const SCH_ITEM* aItem );
    }

    /**
     * Utility functions for error formatting
     */
    namespace ErrorFormatter
    {
        /**
         * Format validation error for API response
         * @param aError ValidationError to format
         * @return Formatted error message string
         */
        std::string FormatValidationError( const ValidationError& aError );

        /**
         * Convert ValidationError to ItemStatus for API response
         * @param aError ValidationError to convert
         * @param aStatus ItemStatus to populate
         */
        void ValidationErrorToItemStatus( const ValidationError& aError,
                                         kiapi::common::commands::ItemStatus& aStatus );
    }
}

#endif // API_SCH_VALIDATION_H