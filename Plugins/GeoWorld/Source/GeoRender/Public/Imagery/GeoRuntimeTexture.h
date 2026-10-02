// =============================================================================
//  GeoRuntimeTexture.h -- Una texture creata a runtime da pixel BGRA gia'
//  pronti, con le loro mipmap. STRATO: UNREAL.
//
//  La usano le ortofoto (Fase 6) e le strade (Fase 8). Era scritta dentro il
//  subsystem delle ortofoto; con un secondo utente e' diventata una funzione,
//  perche' le due copie, prima o poi, avrebbero smesso di coincidere su uno
//  dei dettagli che contano (sRGB, indirizzamento, filtro, mipmap).
// =============================================================================
#pragma once

#include "CoreMinimal.h"

#include <cstdint>
#include <vector>

class UTexture2D;

/**
 * Crea una UTexture2D transitoria PF_B8G8R8A8 e ci copia i pixel e le mipmap.
 *
 * `Mips` sono i livelli dal primo in giu' (meta' lato, un quarto...), come li
 * produce GeoWorld::Tiles::BuildMipChain. Ritorna nullptr se le dimensioni
 * non tornano.
 *
 * La texture e' sRGB, con indirizzamento CLAMP (mai WRAP: il filtro al bordo
 * prenderebbe i pixel del bordo opposto) e il filtro del gruppo World
 * (anisotropico).
 */
GEORENDER_API UTexture2D* CreateGeoRuntimeTexture(int32 Width, int32 Height,
                                                  const std::vector<uint8_t>& Pixels,
                                                  const std::vector<std::vector<uint8_t>>& Mips);

/** Byte occupati in memoria video da una texture BGRA quadrata con le mipmap. */
inline double GeoRuntimeTextureBytes(int32 Side)
{
	return static_cast<double>(Side) * Side * 4.0 * 4.0 / 3.0;
}
