function(engine_set_project_warnings target_name)
  set(CLANG_GCC_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
  )

  if(ENGINE_WARNINGS_AS_ERRORS)
    list(APPEND CLANG_GCC_WARNINGS -Werror)
  endif()

  target_compile_options(${target_name} INTERFACE ${CLANG_GCC_WARNINGS})
endfunction()
