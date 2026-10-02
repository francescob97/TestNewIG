#include "Imagery/GeoImagerySubsystem.h"

#include "Imagery/GeoRuntimeTexture.h"

#include "GeoCoreModule.h"
#include "Lod/GeoQuadtreeSubsystem.h"
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
	/** Il nome con cui le ortofoto si registrano fra i vestitori del terreno. */
	const FName DressOwnerName(TEXT("Ortofoto"));

	/** Byte occupati in memoria video da una texture 256x256 BGRA con le mipmap (+1/3). */
	constexpr double TextureBytes =
		static_cast<double>(Tiles::TilePixels) * Tiles::TilePixels * 4.0 * 4.0 / 3.0;

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
	if (Terrain) { Terrain->ClearDressPredicate(DressOwnerName); }
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

	if (Terrain)
	{
		if (bDrapeEnabled)
		{
			// Una tile si mostra solo vestita: vedi SetDressPredicate. La lambda
			// cattura "this"; la si toglie allo spegnimento e in Deinitialize.
			Terrain->SetDressPredicate(DressOwnerName, [this](const FTileKey& Key) { return IsDressedOrUndressable(Key); });
		}
		else
		{
			// PRIMA di togliere le foto: altrimenti nessuna tile risulterebbe
			// pronta e il terreno sparirebbe.
			Terrain->ClearDressPredicate(DressOwnerName);
		}
	}

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

bool UGeoImagerySubsystem::IsDressedOrUndressable(const FTileKey& Key) const
{
	// Senza dataset non c'e' niente da aspettare.
	if (!Terrain || !Streaming || !Streaming->IsDatasetOpen()) { return true; }

	const IGeoTerrainMeshProvider* Provider = Terrain->GetProvider();
	if (Provider && Provider->IsTileDraped(Key)) { return true; }

	// Non vestita: vale la pena aspettare solo se un'immagine PUO' arrivare,
	// cioe' se il dataset ha la tile allo stesso livello o un suo antenato.
	// Fuori dalla copertura delle ortofoto la tile si mostra grigia, invece di
	// lasciare a schermo il padre per sempre. Gli indici sono tutti in memoria
	// (si caricano all'apertura): sono lookup, non letture da disco.
	const FGeoImageryDataset& Dataset = Streaming->GetDataset();
	const int32 MinLevel = FMath::Max(0, Dataset.GetMinLevel());
	const int32 Top = FMath::Min(static_cast<int32>(Key.Level), Dataset.GetMaxLevel());
	for (int32 Level = Top; Level >= MinLevel; --Level)
	{
		if (Dataset.TileExists(Imagery::AncestorOf(Key, static_cast<uint32>(Level)))) { return false; }
	}
	return true;
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

	// Dopo un teletrasporto le immagini in coda sono quelle del posto da cui
	// si e' partiti: si buttano, come fa il quadtree con le quote.
	if (const UGeoQuadtreeSubsystem* Lod = GetWorld()->GetSubsystem<UGeoQuadtreeSubsystem>())
	{
		if (Lod->GetTeleportCount() != TeleportsSeen)
		{
			TeleportsSeen = Lod->GetTeleportCount();
			Streaming->CancelPendingRequests();
		}
	}

	// Le chiavi arrivano con le tile A SCHERMO per prime, poi quelle nascoste
	// (costruite in anticipo dal piano di residenza, o tenute dopo l'uso).
	// Vestendo in quest'ordine il budget del frame va prima a cio' che si vede;
	// le nascoste si vestono con quello che avanza, e quando compariranno
	// avranno gia' la loro foto.
	TArray<FTileKey> TerrainKeys;
	const int32 VisibleCount = Terrain->GetBuiltTileKeys(TerrainKeys);

	const FImageryAvailability Availability(Streaming);
	const uint32 MinLevel = static_cast<uint32>(FMath::Max(0, Streaming->GetDataset().GetMinLevel()));
	const uint32 MaxLevel = static_cast<uint32>(FMath::Max(0, Streaming->GetDataset().GetMaxLevel()));

	// In riscaldamento (dopo un teletrasporto) si accetta un frame piu' lento
	// per vestire tutto in fretta, come fa il terreno con le mesh.
	int32 Budget = Terrain->IsWarmingUp()
		? FMath::Max(MaxTexturesPerFrame, WarmupTexturesPerFrame) : MaxTexturesPerFrame;
	int32 LevelMin = MAX_int32;
	int32 LevelMax = MIN_int32;

	// Le texture ancora utili in questo frame. Quelle che non compaiono qui
	// vengono buttate in fondo: e' il modo piu' semplice per non accumulare
	// memoria video su tile che non si guardano piu'.
	TSet<uint64> StillUsed;

	for (int32 Index = 0; Index < TerrainKeys.Num(); ++Index)
	{
		const FTileKey& TerrainKey = TerrainKeys[Index];
		const bool bOnScreen = (Index < VisibleCount);

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
			// A schermo prima delle nascoste: anche nella coda del disco.
			Streaming->RequestTile(Request, /*Priority=*/bOnScreen ? 1 : 0);
			++Stats.RichiesteQuestoFrame;
		}

		if (!bFound)
		{
			++Stats.TileSenzaImmagine;
			continue;
		}

		StillUsed.Add(Drape.ImageKey.Pack());

		UTexture2D* Texture = bCheckerboard
			? GetCheckerboardTexture()
			: GetOrCreateTexture(Drape.ImageKey, Budget);

		if (!Texture && !bCheckerboard)
		{
			// Budget del frame finito. Invece di lasciare la tile com'era (cioe'
			// GRIGIA, se e' nuova), si ripiega su un antenato la cui texture
			// esiste gia': costa zero, e la tile ha subito una foto, solo piu'
			// sfocata. Quella giusta arrivera' nei frame successivi.
			for (uint32 Level = Drape.ImageKey.Level; Level > MinLevel && !Texture; )
			{
				--Level;
				const FTileKey Ancestor = Imagery::AncestorOf(TerrainKey, Level);
				if (const TStrongObjectPtr<UTexture2D>* Existing = Textures.Find(Ancestor.Pack()))
				{
					Texture = Existing->Get();
					Drape = Imagery::MakeDrapeTransform(TerrainKey, Level);
					Drape.ImageKey = Ancestor;
					StillUsed.Add(Ancestor.Pack());
					++Stats.RipieghiSuAntenato;
				}
			}
		}

		if (!Texture)
		{
			// Nessuna texture e nessun antenato pronto: la tile resta com'era e
			// ci si riprova al frame dopo. Non si vede grigia: finche' non e'
			// vestita il terreno non la mostra (SetDressPredicate).
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
	const uint64 Packed = ImageKey.Pack();

	if (const TStrongObjectPtr<UTexture2D>* Found = Textures.Find(Packed))
	{
		return Found->Get();
	}

	if (InOutBudget <= 0) { return nullptr; }

	const auto Tile = Streaming->FindLoadedTile(ImageKey);
	if (!Tile || !Tile->IsDecoded()) { return nullptr; }

	// La creazione vera (sRGB, CLAMP, filtro, mipmap) e' in GeoRuntimeTexture.cpp:
	// la usano anche le strade, e le due copie devono restare identiche.
	UTexture2D* Texture = CreateGeoRuntimeTexture(Tile->Width, Tile->Height, Tile->Pixels, Tile->Mips);
	if (!Texture) { return nullptr; }

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

	if (Terrain)
	{
		if (const IGeoTerrainMeshProvider* Provider = Terrain->GetProvider())
		{
			const FString Problem = Provider->GetMaterialProblem();
			if (!Problem.IsEmpty()) { Line(FColor::Red, Problem); }
		}
	}

	Line(FColor::Green, FString::Printf(TEXT("Tile vestite  : %d   senza immagine %d   con la foto di un antenato %d"),
		Stats.TileVestite, Stats.TileSenzaImmagine, Stats.RipieghiSuAntenato));

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
