# CPack's post-build script for the macOS disk image (CMakeLists.txt),
# also run directly with -P by the release pipeline: with a Developer ID
# identity it signs the image, then notarizes and staples it when given
# credentials, either a keychain profile (ELFEED2_NOTARY_PROFILE, from
# "xcrun notarytool store-credentials") or an App Store Connect API key
# in the environment, as in CI: ELFEED2_NOTARY_KEY (the .p8 file),
# ELFEED2_NOTARY_KEY_ID and ELFEED2_NOTARY_ISSUER. Without them the
# image is signed alone.

if(NOT CPACK_ELFEED2_SIGN_IDENTITY MATCHES "^Developer ID Application")
  message(STATUS "notarize: not a Developer ID identity, so left unsigned")
  return()
endif()

if(CPACK_ELFEED2_NOTARY_PROFILE)
  set(auth --keychain-profile ${CPACK_ELFEED2_NOTARY_PROFILE})
elseif(DEFINED ENV{ELFEED2_NOTARY_KEY})
  set(auth --key $ENV{ELFEED2_NOTARY_KEY}
           --key-id $ENV{ELFEED2_NOTARY_KEY_ID}
           --issuer $ENV{ELFEED2_NOTARY_ISSUER})
else()
  set(auth)
endif()

foreach(dmg IN LISTS CPACK_PACKAGE_FILES)
  execute_process(
    COMMAND codesign --force --timestamp
            --sign ${CPACK_ELFEED2_SIGN_IDENTITY} ${dmg}
    COMMAND_ERROR_IS_FATAL ANY)
  if(NOT auth)
    message(STATUS "notarize: ${dmg} signed, but no credentials to notarize it")
    continue()
  endif()

  # notarytool exits 0 whatever Apple decides, so its status is read
  message(STATUS "notarize: submitting ${dmg}, which takes minutes")
  execute_process(
    COMMAND xcrun notarytool submit ${dmg} ${auth} --wait
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT out MATCHES "status: Accepted")
    string(REGEX MATCH "id: ([0-9a-f-]+)" id "${out}")
    message(FATAL_ERROR "notarize: not accepted\n${out}\n"
            "Apple's reasons: xcrun notarytool log ${CMAKE_MATCH_1}, "
            "with the same credentials")
  endif()

  execute_process(COMMAND xcrun stapler staple ${dmg}
                  COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND spctl --assess --type open --context context:primary-signature
            --verbose ${dmg}
    COMMAND_ERROR_IS_FATAL ANY)
  message(STATUS "notarize: ${dmg} notarized and stapled")
endforeach()
