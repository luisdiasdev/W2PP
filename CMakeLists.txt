cmake_minimum_required(VERSION 3.25)

project(w2pp VERSION 1.0.0 LANGUAGES CXX)

# The servers are 32-bit binaries and the on-disk / on-wire layouts depend on
# the Win32 x86 ABI. Refuse any other architecture instead of emitting a
# mismatched binary.
if(NOT WIN32)
  message(FATAL_ERROR "w2pp targets Windows only. Use the x86-debug or x86-release preset.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "w2pp only supports 32-bit x86 (Win32) builds. Configure with "
    "`cmake --preset x86-debug` or `cmake --preset x86-release`.")
endif()

# Match the legacy ABI: Debug uses the debug CRT, Release uses the release CRT.
# This also fixes the old Release project that linked a debug CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(asio CONFIG REQUIRED)

# Legacy sources are ANSI/CP949 and produce many warnings; keep the legacy
# warning level and disable the CRT deprecation warnings during migration.
# Legacy sources predate strict conformance (e.g. `goto` over an
# initialization); keep the permissive mode the old vcxproj build used.
add_compile_options(/W3 /permissive /Zc:strictStrings-)
# WIN32_LEAN_AND_MEAN keeps <Windows.h> from pulling in the old Winsock.h so
# asio can include Winsock2. ASIO_USE_TS_EXECUTOR_AS_DEFAULT works around asio
# 1.38's default any_io_executor constraints failing under MSVC 19.44/C++20.
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _MBCS WIN32 _WINDOWS _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN _WINSOCK_DEPRECATED_NO_WARNINGS ASIO_STANDALONE ASIO_USE_TS_EXECUTOR_AS_DEFAULT)
add_compile_definitions($<$<CONFIG:Debug>:_PACKET_DEBUG>)

# No transitional Win32 libraries: the servers and the shared code no longer
# call user32, gdi32 or winmm. Winsock is linked by the asio headers.

# ---------------------------------------------------------------------------
# Shared core: code compiled into both servers.
# ---------------------------------------------------------------------------

add_library(w2pp_core STATIC
  Code/Basedef.cpp
  Code/AbiGuards.h
  Code/net/FrameCodec.cpp
  Code/net/FrameCodec.h
  Code/net/Protocol.h
  Code/net/Session.cpp
  Code/net/Session.h
  Code/net/Acceptor.cpp
  Code/net/Acceptor.h
  Code/net/Connector.cpp
  Code/net/Connector.h
  Code/net/Connection.cpp
  Code/net/Connection.h
  Code/net/BillingLink.cpp
  Code/net/BillingLink.h
  Code/net/RetryLadder.cpp
  Code/net/RetryLadder.h
  Code/net/LocalAddress.cpp
  Code/net/LocalAddress.h
  Code/host/Runtime.cpp
  Code/host/Runtime.h
  Code/platform/Clock.cpp
  Code/platform/Clock.h
  Code/platform/Diagnostics.cpp
  Code/platform/Diagnostics.h
  Code/platform/File.cpp
  Code/platform/File.h
  Code/platform/FileSystem.cpp
  Code/platform/FileSystem.h
  Code/platform/Path.cpp
  Code/platform/Path.h
)

target_include_directories(w2pp_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Code
)

target_link_libraries(w2pp_core PUBLIC asio::asio)

# ---------------------------------------------------------------------------
# TMSrv (game server)
# ---------------------------------------------------------------------------

set(TMSRV_SOURCES
  Code/TMSrv/App.cpp
  Code/TMSrv/CCastleZakum.cpp
  Code/TMSrv/CItem.cpp
  Code/TMSrv/CMob.cpp
  Code/TMSrv/CNPCGene.cpp
  Code/TMSrv/CReadFiles.cpp
  Code/TMSrv/CUser.cpp
  Code/TMSrv/CWarTower.cpp
  Code/TMSrv/GetFunc.cpp
  Code/TMSrv/imple.cpp
  Code/TMSrv/MobKilled.cpp
  Code/TMSrv/ProcessClientMessage.cpp
  Code/TMSrv/ProcessDBMessage.cpp
  Code/TMSrv/ProcessSecMinTimer.cpp
  Code/TMSrv/SendFunc.cpp
  Code/TMSrv/Server.cpp
  Code/TMSrv/_MSG_AcceptParty.cpp
  Code/TMSrv/_MSG_AccountLogin.cpp
  Code/TMSrv/_MSG_AccountSecure.cpp
  Code/TMSrv/_MSG_Action.cpp
  Code/TMSrv/_MSG_ApplyBonus.cpp
  Code/TMSrv/_MSG_Attack.cpp
  Code/TMSrv/_MSG_Buy.cpp
  Code/TMSrv/_MSG_CapsuleInfo.cpp
  Code/TMSrv/_MSG_Challange.cpp
  Code/TMSrv/_MSG_ChallangeConfirm.cpp
  Code/TMSrv/_MSG_ChangeCity.cpp
  Code/TMSrv/_MSG_CharacterLogin.cpp
  Code/TMSrv/_MSG_CharacterLogout.cpp
  Code/TMSrv/_MSG_CombineItem.cpp
  Code/TMSrv/_MSG_CombineItemAgatha.cpp
  Code/TMSrv/_MSG_CombineItemAilyn.cpp
  Code/TMSrv/_MSG_CombineItemAlquimia.cpp
  Code/TMSrv/_MSG_CombineItemEhre.cpp
  Code/TMSrv/_MSG_CombineItemExtracao.cpp
  Code/TMSrv/_MSG_CombineItemLindy.cpp
  Code/TMSrv/_MSG_CombineItemOdin.cpp
  Code/TMSrv/_MSG_CombineItemShany.cpp
  Code/TMSrv/_MSG_CombineItemTiny.cpp
  Code/TMSrv/_MSG_CreateCharacter.cpp
  Code/TMSrv/_MSG_DeleteCharacter.cpp
  Code/TMSrv/_MSG_DeleteItem.cpp
  Code/TMSrv/_MSG_Deposit.cpp
  Code/TMSrv/_MSG_Deprivate.cpp
  Code/TMSrv/_MSG_DropItem.cpp
  Code/TMSrv/_MSG_GetItem.cpp
  Code/TMSrv/_MSG_GuildAlly.cpp
  Code/TMSrv/_MSG_InviteGuild.cpp
  Code/TMSrv/_MSG_MessageChat.cpp
  Code/TMSrv/_MSG_MessageWhisper.cpp
  Code/TMSrv/_MSG_Motion.cpp
  Code/TMSrv/_MSG_NoViewMob.cpp
  Code/TMSrv/_MSG_PKMode.cpp
  Code/TMSrv/_MSG_PutoutSeal.cpp
  Code/TMSrv/_MSG_Quest.cpp
  Code/TMSrv/_MSG_QuitTrade.cpp
  Code/TMSrv/_MSG_RemoveParty.cpp
  Code/TMSrv/_MSG_ReqBuy.cpp
  Code/TMSrv/_MSG_ReqRanking.cpp
  Code/TMSrv/_MSG_REQShopList.cpp
  Code/TMSrv/_MSG_ReqTeleport.cpp
  Code/TMSrv/_MSG_ReqTradeList.cpp
  Code/TMSrv/_MSG_Restart.cpp
  Code/TMSrv/_MSG_Sell.cpp
  Code/TMSrv/_MSG_SendAutoTrade.cpp
  Code/TMSrv/_MSG_SendReqParty.cpp
  Code/TMSrv/_MSG_SetShortSkill.cpp
  Code/TMSrv/_MSG_SplitItem.cpp
  Code/TMSrv/_MSG_Trade.cpp
  Code/TMSrv/_MSG_TradingItem.cpp
  Code/TMSrv/_MSG_UpdateItem.cpp
  Code/TMSrv/_MSG_UseItem.cpp
  Code/TMSrv/_MSG_War.cpp
  Code/TMSrv/_MSG_Withdraw.cpp
)

set(TMSRV_HEADERS
  Code/Basedef.h
  Code/ItemEffect.h
  Code/TMSrv/CCastleZakum.h
  Code/TMSrv/CItem.h
  Code/TMSrv/CMob.h
  Code/TMSrv/CNPCGene.h
  Code/TMSrv/CReadFiles.h
  Code/TMSrv/CUser.h
  Code/TMSrv/CWarTower.h
  Code/TMSrv/GetFunc.h
  Code/TMSrv/Language.h
  Code/TMSrv/ProcessClientMessage.h
  Code/TMSrv/ProcessDBMessage.h
  Code/TMSrv/SendFunc.h
  Code/TMSrv/Server.h
)

add_executable(tmsrv
  ${TMSRV_SOURCES}
  ${TMSRV_HEADERS}
)

target_include_directories(tmsrv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Code/TMSrv)
target_link_libraries(tmsrv PRIVATE w2pp_core)

# ---------------------------------------------------------------------------
# DBSrv
# ---------------------------------------------------------------------------

set(DBSRV_SOURCES
  Code/DBSrv/App.cpp
  Code/DBSrv/CFileDB.cpp
  Code/DBSrv/CRanking.cpp
  Code/DBSrv/CReadFiles.cpp
  Code/DBSrv/CUser.cpp
  Code/DBSrv/Server.cpp
)

set(DBSRV_HEADERS
  Code/Basedef.h
  Code/ItemEffect.h
  Code/DBSrv/CFileDB.h
  Code/DBSrv/CRanking.h
  Code/DBSrv/CReadFiles.h
  Code/DBSrv/CUser.h
  Code/DBSrv/Server.h
)

add_executable(dbsrv
  ${DBSRV_SOURCES}
  ${DBSRV_HEADERS}
)

target_include_directories(dbsrv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Code/DBSrv)
target_link_libraries(dbsrv PRIVATE w2pp_core)

# ---------------------------------------------------------------------------
# Legacy release tree
#
# The old MSBuild projects wrote into ..\release\<Project>\run. Keep that
# workflow available: the built executables are copied next to their data so
# they can be started from the existing release tree. Override the root with
# -DW2PP_RELEASE_ROOT=<path> (or an empty value to disable the copy).
# ---------------------------------------------------------------------------

set(W2PP_RELEASE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../release" CACHE PATH
    "Legacy release tree that receives the built server executables")

function(w2pp_copy_to_release target folder)
  if(W2PP_RELEASE_ROOT)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${W2PP_RELEASE_ROOT}/${folder}/run"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:${target}>"
              "${W2PP_RELEASE_ROOT}/${folder}/run/"
      COMMENT "Copying ${target} to ${W2PP_RELEASE_ROOT}/${folder}/run"
      VERBATIM)
  endif()
endfunction()

w2pp_copy_to_release(tmsrv TMSRV)
w2pp_copy_to_release(dbsrv DBSRV)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

include(CTest)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(w2pp_tests
    tests/placeholder.cpp
    tests/codec/CodecTests.cpp
    tests/codec/LegacyReferenceTests.cpp
    tests/platform/PlatformTests.cpp
    tests/net/TransportTests.cpp
    tests/net/RetryLadderTests.cpp
    tests/net/BillingTests.cpp
    tests/net/ClientLinkTests.cpp
    tests/host/RuntimeTests.cpp
  )

  target_link_libraries(w2pp_tests PRIVATE Catch2::Catch2WithMain w2pp_core)
  target_compile_definitions(w2pp_tests PRIVATE
    W2PP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )

  catch_discover_tests(w2pp_tests)

  if(WIN32)
    find_program(W2PP_PWSH pwsh)
    if(NOT W2PP_PWSH)
      set(W2PP_PWSH powershell)
    endif()

    add_test(
      NAME platform-dependency-check
      COMMAND ${W2PP_PWSH} -NoProfile -ExecutionPolicy Bypass
              -File "${CMAKE_SOURCE_DIR}/tests/tools/check_platform_dependencies.ps1"
              -CodeDir "${CMAKE_SOURCE_DIR}/Code"
    )
    set_tests_properties(platform-dependency-check PROPERTIES
      LABELS static
      TIMEOUT 120
    )

    add_test(
      NAME dbsrv-loopback
      COMMAND ${W2PP_PWSH} -NoProfile -ExecutionPolicy Bypass
              -File "${CMAKE_SOURCE_DIR}/tests/integration/dbsrv_loopback.ps1"
              -ExeDir "$<TARGET_FILE_DIR:dbsrv>"
              -CommonDir "$<TARGET_FILE_DIR:dbsrv>/../../Common"
    )
    set_tests_properties(dbsrv-loopback PROPERTIES
      LABELS integration
      TIMEOUT 90
    )

    add_test(
      NAME tmsrv-smoke
      COMMAND ${W2PP_PWSH} -NoProfile -ExecutionPolicy Bypass
              -File "${CMAKE_SOURCE_DIR}/tests/integration/tmsrv_smoke.ps1"
              -ExeDir "$<TARGET_FILE_DIR:tmsrv>"
              -CommonDir "$<TARGET_FILE_DIR:tmsrv>/../../Common"
    )
    set_tests_properties(tmsrv-smoke PROPERTIES
      LABELS integration
      TIMEOUT 120
    )
  endif()

  # Login probe: developer tool that plays one _MSG_AccountLogin against a
  # running server and prints the replies.
  add_executable(login_probe
    tests/tools/login_probe.cpp
  )
  target_link_libraries(login_probe PRIVATE w2pp_core)
endif()

