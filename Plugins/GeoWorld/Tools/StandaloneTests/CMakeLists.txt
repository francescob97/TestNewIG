# =============================================================================
#  Harness di test STANDALONE per lo strato puro di GeoCore.
#
#  Non ha niente a che vedere con Unreal: compila direttamente i .cpp di
#  Public/Geo + Private/Geo con un normale compilatore C++ e li esercita.
#
#  Uso:
#     cmake -S Plugins/GeoWorld/Tools/StandaloneTests -B build
#     cmake --build build
#     ./build/geocore_tests
#
#  Perche' esiste, visto che UE ha gia' il suo framework di automazione:
#   - gira in un secondo, senza aprire l'editor e senza una licenza di Windows;
#   - gira in CI su Linux;
#   - dimostra per costruzione che lo strato puro e' davvero puro: se qualcuno
#     ci infila un #include "CoreMinimal.h", questa build si rompe subito.
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(GeoCoreStandaloneTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(GEOCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoCore)

# -Werror=shadow (GCC, Clang): un nome che ne nasconde un altro -- un membro,
# un parametro, una variabile di fuori -- e' un ERRORE, come in Unreal
# (C4456, C4457, C4458). Lo strato puro si compila qui prima che sulla
# macchina Windows: e' qui che conviene accorgersene.

# Nessun .cpp oltre al main: lo strato puro e' header-only (vedi la nota
# "IMPLEMENTAZIONE" in Ellipsoid.h per il perche').
add_executable(geocore_tests main.cpp)

target_include_directories(geocore_tests PRIVATE ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geocore_tests PRIVATE /W4 /fp:precise)
else()
	# -ffast-math NON va usato: romperebbe le assunzioni IEEE su cui poggiano
	# le soglie numeriche di questi test (e la geodesia in generale).
	target_compile_options(geocore_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- GeoTiles (Fase 3): formato delle tile, schema, cache LRU ---------------
set(GEOTILES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles)

add_executable(geotiles_tests geotiles_main.cpp)
target_include_directories(geotiles_tests PRIVATE
	${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geotiles_tests PRIVATE /W4)
else()
	target_compile_options(geotiles_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Quadtree (Fase 4): volumi, culling, selezione LOD ---------------------
set(GEORENDER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoRender)

add_executable(geoquadtree_tests geoquadtree_main.cpp)
target_include_directories(geoquadtree_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geoquadtree_tests PRIVATE /W4)
else()
	target_compile_options(geoquadtree_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Mesh (Fase 5): generazione della geometria e gonne --------------------
add_executable(geomesh_tests geomesh_main.cpp)
target_include_directories(geomesh_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geomesh_tests PRIVATE /W4)
else()
	target_compile_options(geomesh_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Ortofoto (Fase 6): drappeggio, formato, cache template ----------------
add_executable(geoimagery_tests geoimagery_main.cpp)
target_include_directories(geoimagery_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geoimagery_tests PRIVATE /W4)
else()
	target_compile_options(geoimagery_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Strade (Fase 8): formato .gvt e rasterizzatore delle linee -------------
add_executable(georoads_tests georoads_main.cpp)
target_include_directories(georoads_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(georoads_tests PRIVATE /W4)
else()
	target_compile_options(georoads_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Acqua (Fase 9): formato .gwa, scavo, mesh, onde, texture ----------------
# La zlib serve solo qui, per decomprimere il file di prova: in Unreal lo fa
# FCompression, e lo strato puro riceve il payload gia' decompresso.
find_package(ZLIB REQUIRED)
add_executable(geowater_tests geowater_main.cpp)
target_include_directories(geowater_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)
target_link_libraries(geowater_tests PRIVATE ZLIB::ZLIB)

if (MSVC)
	target_compile_options(geowater_tests PRIVATE /W4)
else()
	target_compile_options(geowater_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

add_executable(geoweather_tests geoweather_main.cpp)
target_include_directories(geoweather_tests PRIVATE ${GEORENDER_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geoweather_tests PRIVATE /W4)
else()
	target_compile_options(geoweather_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Vegetazione: formato .gvg, griglia, specie, forme, materiale ------------
add_executable(geovegetation_tests geovegetation_main.cpp)
target_include_directories(geovegetation_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)
target_link_libraries(geovegetation_tests PRIVATE ZLIB::ZLIB)

if (MSVC)
	target_compile_options(geovegetation_tests PRIVATE /W4)
else()
	target_compile_options(geovegetation_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Edifici: formato .gbd, triangolazione, muri, tetti, materiale -----------
add_executable(geobuildings_tests geobuildings_main.cpp)
target_include_directories(geobuildings_tests PRIVATE
	${GEORENDER_DIR}/Public ${GEOTILES_DIR}/Public ${GEOCORE_DIR}/Public)
target_link_libraries(geobuildings_tests PRIVATE ZLIB::ZLIB)

if (MSVC)
	target_compile_options(geobuildings_tests PRIVATE /W4)
else()
	target_compile_options(geobuildings_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

# --- Cielo: sole, luna, ora locale, luce, shader della notte -------------------
add_executable(geosky_tests geosky_main.cpp)
target_include_directories(geosky_tests PRIVATE ${GEORENDER_DIR}/Public ${GEOCORE_DIR}/Public)

if (MSVC)
	target_compile_options(geosky_tests PRIVATE /W4)
else()
	target_compile_options(geosky_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

enable_testing()
add_test(NAME geocore COMMAND geocore_tests)
add_test(NAME geotiles COMMAND geotiles_tests)
add_test(NAME geoquadtree COMMAND geoquadtree_tests)
add_test(NAME geomesh COMMAND geomesh_tests)
add_test(NAME geoimagery COMMAND geoimagery_tests)
add_test(NAME georoads COMMAND georoads_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles/TestData/vector_fixture.gvt)
add_test(NAME geowater COMMAND geowater_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles/TestData/water_fixture.gwa)
add_test(NAME geoweather COMMAND geoweather_tests)
add_test(NAME geovegetation COMMAND geovegetation_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles/TestData/vegetation_fixture.gvg)
add_test(NAME geobuildings COMMAND geobuildings_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles/TestData/buildings_fixture.gbd)
add_test(NAME geosky COMMAND geosky_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles/TestData/stars_fixture.gst)

# --- Entita' (Fase 7): stato, frame, estrapolazione, registro, modelli,
#     memoria condivisa. Due thread nel test del seqlock: serve Threads.
set(GEOENTITIES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoEntities)
find_package(Threads REQUIRED)

add_executable(geoentities_tests geoentities_main.cpp)
target_include_directories(geoentities_tests PRIVATE ${GEOENTITIES_DIR}/Public ${GEOCORE_DIR}/Public)
target_link_libraries(geoentities_tests PRIVATE Threads::Threads)

if (MSVC)
	target_compile_options(geoentities_tests PRIVATE /W4)
else()
	target_compile_options(geoentities_tests PRIVATE -Wall -Wextra -Werror=shadow -O2)
endif()

add_test(NAME geoentities COMMAND geoentities_tests
	${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GeoTiles/TestData/entities_fixture.bin)
