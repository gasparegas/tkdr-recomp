# copy SRC to DST only if DST does not exist (keeps a deployer's own settings)
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
endif()
