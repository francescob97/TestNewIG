#include "Imagery/GeoRuntimeTexture.h"

#include "Engine/Texture2D.h"
#include "TextureResource.h"

UTexture2D* CreateGeoRuntimeTexture(int32 Width, int32 Height,
                                    const std::vector<uint8_t>& Pixels,
                                    const std::vector<std::vector<uint8_t>>& Mips)
{
	if (Width <= 0 || Height <= 0 ||
	    Pixels.size() != static_cast<size_t>(Width) * static_cast<size_t>(Height) * 4)
	{
		return nullptr;
	}

	// ------------------------------------------------------------------
	//  NOTA UE: creazione di una texture a runtime.
	//
	//  CreateTransient fa una texture che non esiste su disco e non finisce nei
	//  pacchetti. I pixel si scrivono bloccando il mip 0; dopo UpdateResource()
	//  i dati sono sulla scheda video.
	//
	//  DUE PARAMETRI CHE NON SONO DETTAGLI:
	//
	//  AddressX/Y = TA_Clamp, non TA_Wrap. Con Wrap il filtro bilineare sul
	//  bordo destro andrebbe a prendere i pixel del bordo SINISTRO della stessa
	//  texture: sul confine fra due tile comparirebbe una riga di colori presi
	//  dall'altra parte del rettangolo. E' un artefatto difficile da
	//  riconoscere se non si sa che esiste.
	//
	//  SRGB = true perche' un JPEG contiene colori gia' in spazio sRGB. Con
	//  false il terreno verrebbe slavato, e la tentazione sarebbe correggerlo
	//  nel materiale invece che qui, che e' il posto giusto. Vale anche per le
	//  strade: il rasterizzatore scrive il colore premoltiplicato codificato
	//  sRGB, e la scheda video lo riporta in luce lineare prima di filtrarlo.
	// ------------------------------------------------------------------
	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!Texture) { return nullptr; }

	Texture->SRGB = true;
	Texture->AddressX = TextureAddress::TA_Clamp;
	Texture->AddressY = TextureAddress::TA_Clamp;
	Texture->NeverStream = true;

	// FILTRO. La prima versione era TF_Bilinear senza mipmap: da lontano il
	// terreno brulicava, e a viste radenti (cioe' sempre, da un aereo) le foto
	// diventavano una poltiglia. Ora ci sono le mipmap, e il filtro lo decide il
	// gruppo "World" delle impostazioni del motore: anisotropico, cioe' capace di
	// campionare di piu' lungo la direzione in cui la superficie si allontana.
	Texture->LODGroup = TextureGroup::TEXTUREGROUP_World;
	Texture->Filter = TextureFilter::TF_Default;

	FTexturePlatformData* Platform = Texture->GetPlatformData();

	void* Destination = Platform->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Destination, Pixels.data(), Pixels.size());
	Platform->Mips[0].BulkData.Unlock();

	// ------------------------------------------------------------------
	//  NOTA UE: le mipmap di una texture creata a runtime.
	//
	//  CreateTransient crea solo il livello 0. Gli altri si aggiungono a mano
	//  alla lista dei mip della FTexturePlatformData, PRIMA di UpdateResource:
	//  e' li' che la risorsa della scheda video viene creata, e prende tutti i
	//  livelli presenti in quel momento. I pixel sono gia' pronti: li ha
	//  calcolati il worker (BuildMipChain), mediando in luce lineare.
	//
	//  TIndirectArray possiede i puntatori che riceve: il new qui non ha un
	//  delete corrispondente perche' lo fa la texture quando viene distrutta.
	// ------------------------------------------------------------------
	int32 MipWidth = Width;
	int32 MipHeight = Height;
	for (const std::vector<uint8_t>& MipPixels : Mips)
	{
		MipWidth = FMath::Max(1, MipWidth / 2);
		MipHeight = FMath::Max(1, MipHeight / 2);
		if (MipPixels.size() != static_cast<size_t>(MipWidth) * MipHeight * 4) { break; }

		FTexture2DMipMap* Mip = new FTexture2DMipMap();
		Mip->SizeX = MipWidth;
		Mip->SizeY = MipHeight;
		Platform->Mips.Add(Mip);

		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* MipData = Mip->BulkData.Realloc(static_cast<int64>(MipPixels.size()));
		FMemory::Memcpy(MipData, MipPixels.data(), MipPixels.size());
		Mip->BulkData.Unlock();
	}

	Texture->UpdateResource();
	return Texture;
}
