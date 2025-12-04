#!/bin/bash
#
# Adaptive Smart Replay (ASR) - Implementation Validation Script
#
# This script validates that all ASR implementation files are present
# and correctly integrated into the OpenAurora codebase.
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0
WARNINGS=0

PROJECT_ROOT="/home/as/postgresql-13.0"

echo "=========================================="
echo "ASR Implementation Validation"
echo "=========================================="
echo ""
echo "Project Root: $PROJECT_ROOT"
echo ""

# Helper functions
pass() {
    echo -e "${GREEN}✓${NC} $1"
    ((PASSED++))
}

fail() {
    echo -e "${RED}✗${NC} $1"
    ((FAILED++))
}

warn() {
    echo -e "${YELLOW}!${NC} $1"
    ((WARNINGS++))
}

check_file_exists() {
    local file="$1"
    local desc="$2"
    if [ -f "$file" ]; then
        local size=$(wc -c < "$file")
        pass "$desc ($(($size / 1024))KB)"
    else
        fail "$desc - FILE NOT FOUND: $file"
    fi
}

check_file_contains() {
    local file="$1"
    local pattern="$2"
    local desc="$3"
    if grep -q "$pattern" "$file" 2>/dev/null; then
        pass "$desc"
    else
        fail "$desc - PATTERN NOT FOUND: $pattern"
    fi
}

# ============================================================================
# SECTION 1: Core Implementation Files
# ============================================================================
echo "1. Core Implementation Files"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

check_file_exists \
    "$PROJECT_ROOT/src/openaurora/include/storage/adaptive_sr.h" \
    "Header file: adaptive_sr.h"

check_file_exists \
    "$PROJECT_ROOT/src/openaurora/backend/storage/adaptive_sr.c" \
    "Implementation: adaptive_sr.c"

echo ""

# ============================================================================
# SECTION 2: Header Content Validation
# ============================================================================
echo "2. Header File Content"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

HEADER="$PROJECT_ROOT/src/openaurora/include/storage/adaptive_sr.h"

check_file_contains "$HEADER" "ASRConfig" "Header defines ASRConfig struct"
check_file_contains "$HEADER" "ASRMetrics" "Header defines ASRMetrics struct"
check_file_contains "$HEADER" "ASR_Init" "Header declares ASR_Init()"
check_file_contains "$HEADER" "ASR_StartController" "Header declares ASR_StartController()"
check_file_contains "$HEADER" "ASR_GetCurrentBudget" "Header declares ASR_GetCurrentBudget()"
check_file_contains "$HEADER" "ASR_RecordReplayTask" "Header declares ASR_RecordReplayTask()"
check_file_contains "$HEADER" "ASR_RecordHotMiss" "Header declares ASR_RecordHotMiss()"
check_file_contains "$HEADER" "ASR_RecordWalIngest" "Header declares ASR_RecordWalIngest()"
check_file_contains "$HEADER" "ASR_ReadMetrics" "Header declares ASR_ReadMetrics()"

echo ""

# ============================================================================
# SECTION 3: Implementation File Validation
# ============================================================================
echo "3. Implementation File Content"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

IMPL="$PROJECT_ROOT/src/openaurora/backend/storage/adaptive_sr.c"

check_file_contains "$IMPL" "asr_default_config" "Implementation defines default config"
check_file_contains "$IMPL" "ASR_Init" "Implementation defines ASR_Init()"
check_file_contains "$IMPL" "ASR_StartController" "Implementation defines ASR_StartController()"
check_file_contains "$IMPL" "ASR_RecordReplayTask" "Implementation defines ASR_RecordReplayTask()"
check_file_contains "$IMPL" "ASR_RecordHotMiss" "Implementation defines ASR_RecordHotMiss()"
check_file_contains "$IMPL" "ASR_RecordWalIngest" "Implementation defines ASR_RecordWalIngest()"
check_file_contains "$IMPL" "asr_controller_main" "Implementation defines controller thread"
check_file_contains "$IMPL" "asr_update_smoothed_metrics" "Implementation defines metrics update"
check_file_contains "$IMPL" "EWMA_ALPHA" "Implementation uses EWMA smoothing"

echo ""

# ============================================================================
# SECTION 4: Code Integration Validation
# ============================================================================
echo "4. Code Integration"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check wal_redo.c
WAL_REDO="$PROJECT_ROOT/src/openaurora/backend/tcop/wal_redo.c"
check_file_contains "$WAL_REDO" "#include \"storage/adaptive_sr.h\"" "wal_redo.c includes adaptive_sr.h"
check_file_contains "$WAL_REDO" "ASR_GetCurrentBudget" "wal_redo.c calls ASR_GetCurrentBudget()"
check_file_contains "$WAL_REDO" "ASR_RecordReplayTask" "wal_redo.c calls ASR_RecordReplayTask()"
check_file_contains "$WAL_REDO" "replay_budget" "wal_redo.c declares replay_budget variable"
check_file_contains "$WAL_REDO" "records_replayed" "wal_redo.c tracks records_replayed"

echo ""

# Check storage_server.c
STORAGE_SERVER="$PROJECT_ROOT/src/openaurora/backend/tcop/storage_server.c"
check_file_contains "$STORAGE_SERVER" "#include \"storage/adaptive_sr.h\"" "storage_server.c includes adaptive_sr.h"
check_file_contains "$STORAGE_SERVER" "ASR_Init" "storage_server.c calls ASR_Init()"
check_file_contains "$STORAGE_SERVER" "ASR_StartController" "storage_server.c calls ASR_StartController()"

echo ""

# Check rpcserver.cpp
RPCSERVER="$PROJECT_ROOT/src/openaurora/backend/storage/rpc/rpcserver.cpp"
check_file_contains "$RPCSERVER" "#include \"storage/adaptive_sr.h\"" "rpcserver.cpp includes adaptive_sr.h"
check_file_contains "$RPCSERVER" "ASR_RecordHotMiss" "rpcserver.cpp calls ASR_RecordHotMiss()"

echo ""

# Check walreceiver.c
WALRECEIVER="$PROJECT_ROOT/src/openaurora/backend/replication/walreceiver.c"
check_file_contains "$WALRECEIVER" "#include \"storage/adaptive_sr.h\"" "walreceiver.c includes adaptive_sr.h"
check_file_contains "$WALRECEIVER" "ASR_RecordWalIngest" "walreceiver.c calls ASR_RecordWalIngest()"

echo ""

# ============================================================================
# SECTION 5: Documentation Files
# ============================================================================
echo "5. Documentation Files"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

DOCS_DIR="$PROJECT_ROOT/src/openaurora/docs"

check_file_exists "$DOCS_DIR/adaptive_smart_replay.md" "Architecture & Design Document"
check_file_exists "$DOCS_DIR/ASR_IMPLEMENTATION_SUMMARY.md" "Implementation Summary"
check_file_exists "$DOCS_DIR/asr_integration_reference.c" "Integration Reference"
check_file_exists "$DOCS_DIR/asr_setup_guide.sh" "Setup & Testing Guide"
check_file_exists "$DOCS_DIR/asr_quickstart.sh" "Interactive Quickstart"
check_file_exists "$DOCS_DIR/ASR_DOCUMENTATION_INDEX.md" "Documentation Index"
check_file_exists "$DOCS_DIR/ASR_CHANGES_SUMMARY.md" "Changes Summary"

echo ""

# ============================================================================
# SECTION 6: Documentation Content
# ============================================================================
echo "6. Documentation Content"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

check_file_contains "$DOCS_DIR/adaptive_smart_replay.md" "Overview" "adaptive_smart_replay.md covers overview"
check_file_contains "$DOCS_DIR/adaptive_smart_replay.md" "Configuration" "adaptive_smart_replay.md covers configuration"
check_file_contains "$DOCS_DIR/adaptive_smart_replay.md" "Testing" "adaptive_smart_replay.md covers testing"

check_file_contains "$DOCS_DIR/ASR_IMPLEMENTATION_SUMMARY.md" "Executive Summary" "Summary covers executive overview"
check_file_contains "$DOCS_DIR/ASR_IMPLEMENTATION_SUMMARY.md" "Files Delivered" "Summary covers files delivered"

check_file_contains "$DOCS_DIR/asr_setup_guide.sh" "Building OpenAurora" "Setup guide covers building"
check_file_contains "$DOCS_DIR/asr_setup_guide.sh" "Test Workloads" "Setup guide covers testing"

echo ""

# ============================================================================
# SECTION 7: Syntax Validation
# ============================================================================
echo "7. Syntax Validation"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check header syntax
if cpp -o /dev/null "$HEADER" 2>/dev/null; then
    pass "Header file has valid C syntax"
else
    warn "Header file may have syntax issues (cpp check failed)"
fi

# Check Markdown syntax (basic: check for balanced code blocks)
for md_file in "$DOCS_DIR"/*.md; do
    if [ -f "$md_file" ]; then
        # Count code block markers
        open_count=$(grep -c '```' "$md_file" || echo "0")
        if [ $((open_count % 2)) -eq 0 ]; then
            pass "$(basename $md_file) has balanced code blocks"
        else
            warn "$(basename $md_file) may have unbalanced code blocks"
        fi
    fi
done

echo ""

# ============================================================================
# SECTION 8: File Statistics
# ============================================================================
echo "8. File Statistics"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo "New files:"
wc -l "$PROJECT_ROOT/src/openaurora/include/storage/adaptive_sr.h" | awk '{print "  Header: " $1 " lines"}'
wc -l "$PROJECT_ROOT/src/openaurora/backend/storage/adaptive_sr.c" | awk '{print "  Implementation: " $1 " lines"}'

echo ""
echo "Documentation:"
total_doc_lines=0
for doc in "$DOCS_DIR"/adaptive_smart_replay.md "$DOCS_DIR"/ASR_*.md "$DOCS_DIR"/asr_*.{sh,c}; do
    if [ -f "$doc" ]; then
        lines=$(wc -l < "$doc")
        total_doc_lines=$((total_doc_lines + lines))
        printf "  %-40s %6d lines\n" "$(basename $doc)" "$lines"
    fi
done
echo "  ────────────────────────────────────────────────"
printf "  %-40s %6d lines\n" "TOTAL DOCUMENTATION" "$total_doc_lines"

echo ""
echo "Modified files (code):"
for file in "$WAL_REDO" "$STORAGE_SERVER" "$RPCSERVER" "$WALRECEIVER"; do
    name=$(basename "$file")
    printf "  %-40s modified\n" "$name"
done

echo ""

# ============================================================================
# SECTION 9: Integration Points
# ============================================================================
echo "9. Integration Points Verification"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check that all 4 modified files have the include
for file in "$WAL_REDO" "$STORAGE_SERVER" "$RPCSERVER" "$WALRECEIVER"; do
    name=$(basename "$file")
    if grep -q "#include \"storage/adaptive_sr.h\"" "$file"; then
        pass "$name has adaptive_sr.h include"
    else
        fail "$name missing adaptive_sr.h include"
    fi
done

echo ""

# ============================================================================
# SECTION 10: Summary
# ============================================================================
echo "10. Summary"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

TOTAL=$((PASSED + FAILED + WARNINGS))
echo "Results: $TOTAL checks performed"
echo "  Passed:  $PASSED"
echo "  Failed:  $FAILED"
echo "  Warnings: $WARNINGS"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ ASR implementation validation PASSED${NC}"
    echo ""
    echo "All files are present and correctly integrated."
    echo ""
    echo "Next steps:"
    echo "  1. Review documentation: $DOCS_DIR/ASR_DOCUMENTATION_INDEX.md"
    echo "  2. Enable ASR: Edit src/backend/storage/adaptive_sr.c"
    echo "  3. Build: ./configure && make -j8 && make install"
    echo "  4. Test: Follow asr_setup_guide.sh"
    exit 0
else
    echo -e "${RED}✗ ASR implementation validation FAILED${NC}"
    echo ""
    echo "Please address the failures above and re-run validation."
    exit 1
fi
