# Writes OUT, a source file with the time of this build (shown in About), at every build;
# rewritten only when it differs, so nothing else is built again. SOURCE_DATE_EPOCH, when
# set, is the time instead (for reproducible builds).
string(TIMESTAMP now "%Y-%m-%dT%H:%M:%SZ" UTC)
set(text "// Written by cmake/BuildTime.cmake at every build\nextern const char* const kOurNotesBuildTime;\nconst char* const kOurNotesBuildTime = \"${now}\";\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
