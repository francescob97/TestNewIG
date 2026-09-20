#include "Imagery/GeoImagerySubsystem.h"

#include "GeoCoreModule.h"
#include "Streaming/GeoImageryStreamingSubsystem.h"
#include "Terrain/GeoTerrainMeshProvider.h"
#include "Terrain/GeoTerrainSubsystem.h"
#include "Tiles/ImageTileFormat.h"

#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"

using namespace GeoWorld;
using GeoWorld::Imagery::FDrapeTransform;
using GeoWorld::Tiles::FTileKey;

namespace
{
	FORCEINLINE uint64 PackKey(const FTileKey& Key)
	{
		return (static_cast<uint64>(Key.Level) << 58)
		     ^ (static_cast<uint64>(Key.Y) << 29)
		     ^ static_cast<uint64>(Key.X);
	}

	/** Byte occupati in memoria video da una texture 256x256 BGRA senza mipmap. */
	constexpr double TextureBytes =
		static_cast<double>(Tiles::TilePixels) * Tiles::TilePixels * 4.0;

	/**
	 * Adatta lo streaming all'interfaccia che la matematica del drappeggio si
	 * aspetta.
	 *
	 * E' lo stesso schema della Fase 4 con FStreamingAvailability: la logica
	 * pura non deve sapere niente ne' della cache ne' del disco, cosi' i test le
	 * possono dare una disponibilita' finta e verificarne le scelte senza
	 * costruire un dataset.
	 */
	class FImageryAvailability : public Imagery::IImageAvailability
	{
	public:
		explicit FImageryAvailability(UGeoImageryStreamingSubsystem* InStreaming)
			: Streaming(InStreaming) {}

		virtual bool ImageExists(const FTileKey& Key) const override
		{
			return Streaming && Streaming->GetDataset().TileExists(Key);
		}

		virtual bool ImageIsResident(const FTileKey& Key) const override
		{
			return Streaming && Streaming->FindLoadedTile(Key) != nullptr;
		}

	private:
		UGeoImageryStreamingSubsystem* Streaming = nullptr;
	};
}

// ============================================================================
//  Ciclo di vita
// ============================================================================

bool UGeoImagerySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer)) { return false; }
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game
	              || World->WorldType == EWorldType::PIE
	              || World->WorldType == EWorldType::Editor);
}

void UGeoImagerySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Collection.InitializeDependency<UGeoImageryStreamingSubsystem>();
	Collection.InitializeDependency<UGeoTerrainSubsystem>();

	UWorld* World = GetWorld();
	Streaming = World->GetSubsystem<UGeoImageryStreamingSubsystem>();
	Terrain = World->GetSubsystem<UGeoTerrainSubsystem>();
}

void UGeoImagerySubsystem::Deinitialize()
{
	ReleaseAllTextures();
	Super::Deinitialize();
}

TStatId UGeoImagerySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGeoImagerySubsystem, STATGROUP_Tickables);
}

void UGeoImagerySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bDrapeEnabled) { SynchroniseWithTerrain(); }
	if (bShowDebugOverlay) { DrawDebugOverlay(); }
}

void UGeoImagerySubsystem::SetEnabled(bool bInEnabled)
{
	if (bDrapeEnabled == bInEnabled) { return; }
	bDrapeEnabled = bInEnabled;

	if (!bDrapeEnabled && Terrain)
	{
		// Spegnendo il drappeggio si toglie la texture a tutte le tile, cosi' il
		// terreno torna grigio invece di restare congelato sull'ultima immagine.
		if (IGeoTerrainMeshProvider* Provider = Terrain->GetProvider())
		{
			TArray<FTileKey> Keys;
			Terrain->GetBuiltTileKeys(Keys);
			for (const FTileKey& Key : Keys)
			{
				Provider->SetTileDrape(Key, nullptr, FDrapeTransform{});
			}
		}
		ReleaseAllTextures();
	}
}

void UGeoImagerySubsystem::SetCheckerboard(bool bInChecker)
{
	if (bCheckerboard == bInChecker) { return; }
	bCheckerboard = bInChecker;
	// Le texture vecchie non servono piu': cambiando modalita' cambia tutto
	// cio' che va disegnato.
	Textures.Empty();
}

void UGeoImagerySubsystem::ReleaseAllTextures()
{
	Textures.Empty();
	CheckerTexture.Reset();
}

// ============================================================================
//  Il lavoro vero
// ============================================================================

void UGeoImagerySubsystem::SynchroniseWithTerrain()
{
	Stats = FGeoImageryStats();

	if (!Terrain || !Streaming || !Streaming->IsDatasetOpen()) { return; }

	IGeoTerrainMeshProvider* Provider = Terrain->GetProvider();
	if (!Provider) { return; }

	TArray<FTileKey> TerrainKeys;
	Terrain->GetBuiltTileKeys(TerrainKeys);

	const FImageryAvailability Availability(Streaming);
	const uint32 MinLevel = static_cast<uint32>(FMath::Max(0, Streaming->GetDataset().GetMinLevel()));
	const uint32 MaxLevel = static_cast<uint32>(FMath::Max(0, Streaming->GetDataset().GetMaxLevel()));

	int32 Budget = MaxTexturesPerFrame;
	int32 LevelMin = MAX_int32;
	int32 LevelMax = MIN_int32;

	// Le texture ancora utili in questo frame. Quelle che non compaiono qui
	// vengono buttate in fondo: e' il modo piu' semplice per non accumulare
	// memoria video su tile che non si guardano piu'.
	TSet<uint64> StillUsed;

	for (const FTileKey& TerrainKey : TerrainKeys)
	{
		FDrapeTransform Drape;
		FTileKey Request;
		bool bHasRequest = false;

		const bool bFound = Imagery::ChooseDrape(
			TerrainKey, MinLevel, MaxLevel, Availability, Drape, Request, bHasRequest);

		// Si chiede SEMPRE la tile giusta, anche quando se ne sta gia' usando
		// una grossolana: e' cio' che fa migliorare il drappeggio invece di
		// lasciarlo sfocato per sempre.
		if (bHasRequest)
		{
			Streaming->RequestTile(Request, /*Priority=*/0);
			++Stats.RichiesteQuestoFrame;
		}

		if (!bFound)
		{
			++Stats.TileSenzaImmagine;
			continue;
		}

		StillUsed.Add(PackKey(Drape.ImageKey));

		UTexture2D* Texture = bCheckerboard
			? GetCheckerboardTexture()
			: GetOrCreateTexture(Drape.ImageKey, Budget);

		if (!Texture)
		{
			// Texture non creata perche' il budget del frame e' finito: la tile
			// resta com'era e ci si riprova al frame dopo.
			++Stats.TileSenzaImmagine;
			continue;
		}

		Provider->SetTileDrape(TerrainKey, Texture, Drape);
		++Stats.TileVestite;

		if (!Drape.IsIdentity()) { ++Stats.TileConImmagineGrossolana; }
		LevelMin = FMath::Min(LevelMin, static_cast<int32>(Drape.ImageKey.Level));
		LevelMax = FMath::Max(LevelMax, static_cast<int32>(Drape.ImageKey.Level));
	}

	if (!bCheckerboard)
	{
		for (auto It = Textures.CreateIterator(); It; ++It)
		{
			if (!StillUsed.Contains(It.Key())) { It.RemoveCurrent(); }
		}
	}

	Stats.TextureInMemoria = bCheckerboard ? 1 : Textures.Num();
	Stats.MemoriaTextureMB = static_cast<float>(Stats.TextureInMemoria * TextureBytes
		/ (1024.0 * 1024.0));
	Stats.LivelloImmagineMin = (LevelMin == MAX_int32) ? 0 : LevelMin;
	Stats.LivelloImmagineMax = (LevelMax == MIN_int32) ? 0 : LevelMax;
}

UTexture2D* UGeoImagerySubsystem::GetOrCreateTexture(const FTileKey& ImageKey, int32& InOutBudget)
{
	const uint64 Packed = PackKey(ImageKey);

	if (const TStrongObjectPtr<UTexture2D>* Found = Textures.Find(Packed))
	{
		return Found->Get();
	}

	if (InOutBudget <= 0) { return nullptr; }

	const auto Tile = Streaming->FindLoadedTile(ImageKey);
	if (!Tile || !Tile->IsDecoded()) { return nullptr; }

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
	//  nel materiale invece che qui, che e' il posto giusto.
	// ------------------------------------------------------------------
	UTexture2D* Texture = UTexture2D::CreateTransient(Tile->Width, Tile->Height, PF_B8G8R8A8);
	if (!Texture) { return nullptr; }

	Texture->SRGB = true;
	Texture->Filter = TextureFilter::TF_Bilinear;
	Texture->AddressX = TextureAddress::TA_Clamp;
	Texture->AddressY = TextureAddress::TA_Clamp;
	Texture->NeverStream = true;

	void* Destination = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Destination, Tile->Pixels.data(), Tile->Pixels.size());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();

	Textures.Add(Packed, TStrongObjectPtr<UTexture2D>(Texture));
	--InOutBudget;
	++Stats.CreateQuestoFrame;

	return Texture;
}

UTexture2D* UGeoImagerySubsystem::GetCheckerboardTexture()
{
	if (CheckerTexture.IsValid()) { return CheckerTexture.Get(); }

	// Scacchiera con quadri da 16 pixel: abbastanza grandi da vedersi da
	// lontano, abbastanza piccoli da far notare un disallineamento al bordo.
	constexpr int32 Side = Tiles::TilePixels;
	constexpr int32 Square = 16;

	UTexture2D* Texture = UTexture2D::CreateTransient(Side, Side, PF_B8G8R8A8);
	if (!Texture) { return nullptr; }

	Texture->SRGB = true;
	Texture->Filter = TextureFilter::TF_Nearest;   // bordi netti: e' il punto
	Texture->AddressX = TextureAddress::TA_Clamp;
	Texture->AddressY = TextureAddress::TA_Clamp;
	Texture->NeverStream = true;

	uint8* Pixels = static_cast<uint8*>(
		Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));

	for (int32 Row = 0; Row < Side; ++Row)
	{
		for (int32 Column = 0; Column < Side; ++Column)
		{
			const bool bLight = ((Row / Square) + (Column / Square)) % 2 == 0;
			const uint8 Value = bLight ? 220 : 40;

			// Il bordo della tile e' rosso: e' la riga che deve combaciare con
			// quella della tile accanto. Se si vede doppia o sfalsata, il
			// problema e' nelle UV.
			const bool bEdge = Row == 0 || Column == 0 || Row == Side - 1 || Column == Side - 1;

			uint8* Pixel = Pixels + (static_cast<int64>(Row) * Side + Column) * 4;
			Pixel[0] = bEdge ? 0   : Value;      // B
			Pixel[1] = bEdge ? 0   : Value;      // G
			Pixel[2] = bEdge ? 255 : Value;      // R
			Pixel[3] = 255;
		}
	}

	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();

	CheckerTexture.Reset(Texture);
	return Texture;
}

void UGeoImagerySubsystem::DrawDebugOverlay()
{
	if (!GEngine) { return; }

	int32 Key = 0x6EA0;
	auto Line = [&Key](const FColor& Colour, const FString& Text)
	{
		GEngine->AddOnScreenDebugMessage(Key++, 0.0f, Colour, Text);
	};

	Line(FColor::Cyan, TEXT("--- GeoWorld | Fase 6: ortofoto ---"));

	if (!bDrapeEnabled)
	{
		Line(FColor::Yellow, TEXT("Drappeggio spento. Attiva con: geo.Imagery.Enable 1"));
		return;
	}

	if (!Streaming || !Streaming->IsDatasetOpen())
	{
		Line(FColor::Red, TEXT("Nessun dataset di immagini aperto: geo.Imagery.Open <cartella>"));
		return;
	}

	Line(FColor::Green, FString::Printf(TEXT("Tile vestite  : %d   senza immagine %d"),
		Stats.TileVestite, Stats.TileSenzaImmagine));

	// Se molte tile usano un'immagine piu' grossolana del proprio livello, o la
	// piramide delle immagini e' piu' bassa (normale) oppure lo streaming non
	// sta stando al passo (da guardare).
	Line(Stats.TileConImmagineGrossolana > Stats.TileVestite / 2 ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Livelli usati : %d..%d   grossolane %d"),
			Stats.LivelloImmagineMin, Stats.LivelloImmagineMax,
			Stats.TileConImmagineGrossolana));

	Line(FColor::White, FString::Printf(TEXT("Texture       : %d   %.1f MB video   +%d questo frame"),
		Stats.TextureInMemoria, Stats.MemoriaTextureMB, Stats.CreateQuestoFrame));

	const FGeoImageryStreamingStats StreamStats = Streaming->GetStats();
	Line(FColor::White, FString::Printf(
		TEXT("Streaming     : %d in cache, %d in volo, %.1f/%.0f MB"),
		StreamStats.TileResidenti, StreamStats.TileInVolo,
		StreamStats.MemoriaMB, StreamStats.BudgetMB));

	Line(StreamStats.TempoMedioDecodificaMs > 5.0f ? FColor::Yellow : FColor::White,
		FString::Printf(TEXT("Per tile      : lettura %.2f ms, decodifica %.2f ms (sul worker)"),
			StreamStats.TempoMedioCaricamentoMs, StreamStats.TempoMedioDecodificaMs));

	if (bCheckerboard)
	{
		Line(FColor::Magenta, TEXT("SCACCHIERA attiva: geo.Imagery.Checker 0 per tornare alle foto"));
	}
}
