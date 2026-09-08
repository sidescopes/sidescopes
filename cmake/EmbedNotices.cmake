# Generate byte arrays so dependency text survives quotes, encodings and the
# compiler's string-literal length limit without any runtime decoding.
function(embed_notice name hex)
    string(MAKE_C_IDENTIFIER "${name}" identifier)
    file(APPEND "${NOTICE_CPP}.tmp" "constexpr char notice_${identifier}[] = {\n")
    string(LENGTH "${hex}" length)
    if(length GREATER 0)
        math(EXPR last "${length} - 1")
        foreach(offset RANGE 0 ${last} 32)
            string(SUBSTRING "${hex}" ${offset} 32 chunk)
            string(REGEX REPLACE "(..)" "'\\\\x\\1'," chunk "${chunk}")
            file(APPEND "${NOTICE_CPP}.tmp" "    ${chunk}\n")
        endforeach()
    endif()
    file(APPEND "${NOTICE_CPP}.tmp" "    '\\0'};\n")
    string(REPLACE "-" " " label "${name}")
    if(name STREQUAL "SideScopes-GPL")
        set(label "SideScopes")
    endif()
    set_property(GLOBAL APPEND_STRING PROPERTY NOTICE_ENTRIES
        "        {\"${label}\", {notice_${identifier}, sizeof(notice_${identifier}) - 1}},\n")
endfunction()

function(begin_embedded_notices)
    get_filename_component(directory "${NOTICE_CPP}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${NOTICE_CPP}.tmp"
        "#include \"app/license_notices.h\"\n\nnamespace sidescopes {\nnamespace {\n")
    set_property(GLOBAL PROPERTY NOTICE_ENTRIES "")
endfunction()

function(finish_embedded_notices)
    get_property(entries GLOBAL PROPERTY NOTICE_ENTRIES)
    if(NOT entries)
        message(FATAL_ERROR "No distribution notices collected")
    endif()
    file(APPEND "${NOTICE_CPP}.tmp"
        "}  // namespace\n\nstd::span<const LicenseNotice> licenseNotices()\n{\n"
        "    static constexpr LicenseNotice Notices[] = {\n${entries}    };\n"
        "    return Notices;\n}\n}  // namespace sidescopes\n")
    configure_file("${NOTICE_CPP}.tmp" "${NOTICE_CPP}" COPYONLY)
    file(REMOVE "${NOTICE_CPP}.tmp")
endfunction()
