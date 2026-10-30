// =============================================================================
//  CloudShader.inl -- La densita' delle nuvole: il corpo del nodo Custom di
//  M_GeoClouds. HLSL E C++ INSIEME.
//
//  Il testo sta dentro GEO_CLOUD_SHADER( ... ). Chi lo include decide cosa
//  farne definendo la macro:
//   - la factory dei materiali (GeoTerrainMaterialFactory.cpp) lo trasforma in
//     una stringa (#__VA_ARGS__) e la mette nel nodo Custom;
//   - i test (geoweather_main.cpp) lo lasciano com'e' (__VA_ARGS__) dentro una
//     funzione C++, con dei tipi che imitano quelli di HLSL (HlslShim.h).
//  Cosi' quello che i test provano e' esattamente quello che gira sulla
//  scheda video.
//
//  REGOLE per scrivere qui, perche' valga nei due linguaggi: niente swizzle
//  oltre a .x .y .z .w; niente "#" (la stringa); niente funzioni libere (in
//  un nodo Custom si scrive solo un CORPO di funzione: le funzioni d'aiuto
//  stanno in una struct, ed e' il trucco classico dei nodi Custom); da float a
//  uint sempre passando per int (un float negativo in uint non e' definito).
//  I commenti spariscono: il preprocessore li toglie prima della stringa.
//
//  IL RUMORE E' PERIODICO. Ogni ottava avvolge le celle con un suo periodo
//  (Clouds.h): in orizzontale 1500 km (FairPatternPeriodM: 1000 celle da
//  1,5 km, 2900 per l'ottava a x2,9, 5360 per il dettaglio a x5,36), nel tempo
//  un giorno (CloudTimePeriodS: 48, 96 e 144 celle). Cosi' quando il C++
//  riporta l'origine del disegno o l'orologio dentro il periodo (per tenere
//  piccoli i numeri di un float) il cielo non cambia di un pixel. Il +0,5
//  prima di dividere: la divisione della scheda video non e' esatta, e una
//  cella intera divisa per il periodo potrebbe cadere appena sotto l'intero.
//
//  Ingressi (i nomi del nodo Custom): Pos (posizione del campione, cm, assi
//  di Unreal), Map, Shift, Layer, Look, Flash, Fair (Clouds.h,
//  FCloudShaderParameters), Weather + WeatherSampler (la mappa meteo).
//  Uscita: float4(estinzione per metro, luce del lampo, densita' 0..1,
//  copertura di pioggia della colonna).
// =============================================================================
GEO_CLOUD_SHADER(
struct FGeoCloudNoise
{
	float Hash(float3 Cell, float3 Period)
	{
		float3 Wrapped = Cell - Period * floor((Cell + float3(0.5, 0.5, 0.5)) / Period);
		uint Hx = uint(int(Wrapped.x)) * 1597334677u;
		uint Hy = uint(int(Wrapped.y)) * 3812015801u;
		uint Hz = uint(int(Wrapped.z)) * 2798796415u;
		uint Mixed = (Hx ^ Hy ^ Hz) * 2246822519u;
		Mixed = Mixed ^ (Mixed >> 15u);
		Mixed = Mixed * 3266489917u;
		Mixed = Mixed ^ (Mixed >> 16u);
		return float(Mixed) * 2.3283064e-10;
	}

	float Value(float3 Point, float3 Period)
	{
		float3 Cell = floor(Point);
		float3 Inside = Point - Cell;
		float3 Smooth = Inside * Inside * (float3(3.0, 3.0, 3.0) - Inside * 2.0);
		float C000 = Hash(Cell, Period);
		float C100 = Hash(Cell + float3(1.0, 0.0, 0.0), Period);
		float C010 = Hash(Cell + float3(0.0, 1.0, 0.0), Period);
		float C110 = Hash(Cell + float3(1.0, 1.0, 0.0), Period);
		float C001 = Hash(Cell + float3(0.0, 0.0, 1.0), Period);
		float C101 = Hash(Cell + float3(1.0, 0.0, 1.0), Period);
		float C011 = Hash(Cell + float3(0.0, 1.0, 1.0), Period);
		float C111 = Hash(Cell + float3(1.0, 1.0, 1.0), Period);
		float Low = lerp(lerp(C000, C100, Smooth.x), lerp(C010, C110, Smooth.x), Smooth.y);
		float High = lerp(lerp(C001, C101, Smooth.x), lerp(C011, C111, Smooth.x), Smooth.y);
		return lerp(Low, High, Smooth.z);
	}
};
FGeoCloudNoise CloudNoise;

// In metri; la quota sul mare come per il suolo bagnato: Z, piu' la curvatura
// della Terra, meno il geoide all'origine.
float3 Metres = Pos * 0.01;
float Altitude = Metres.z + (Metres.x * Metres.x + Metres.y * Metres.y) / 12742000.0 - Look.y;

// La colonna dalla mappa meteo: dove sta adesso cio' che il vento ha portato
// qui da quando la mappa e' stata fatta. U verso est, V verso nord.
float North = Metres.x - Map.x - Shift.x;
float East = Metres.y - Map.y - Shift.y;
float U = East / Map.z + 0.5;
float V = North / Map.z + 0.5;
float4 Column = float4(0.0, 0.0, 0.0, 0.0);
if (U > 0.0 && U < 1.0 && V > 0.0 && V < 1.0)
{
	Column = Texture2DSampleLevel(Weather, WeatherSampler, float2(U, V), 0.0);
}

// Le nubi di pioggia: una base netta in basso, la cima che la colonna dice.
float RainBase = Look.z;
float RainTop = lerp(RainBase + 600.0, Layer.y, Column.y);
float RainHeight = (Altitude - RainBase) / max(RainTop - RainBase, 1.0);
float RainCloud = Column.x * saturate(RainHeight * 8.0) * saturate((1.0 - RainHeight) * 4.0);

// I cumuli: chiazze di 1,5 km che viaggiano con il vento in quota. La base
// e' piatta (li' l'aria che sale condensa), la cima a cupola: piu' alta dove
// la chiazza e' piu' forte. Layer.z e' la soglia che lascia sopra di se' la
// copertura chiesta (FairCloudThreshold).
float FairX = (Metres.x + Fair.x) / 1500.0;
float FairY = (Metres.y + Fair.y) / 1500.0;
float Patch = CloudNoise.Value(float3(FairX, FairY, Look.w / 1800.0), float3(1000.0, 1000.0, 48.0)) * 0.7
            + CloudNoise.Value(float3(FairX * 2.9 + 11.0, FairY * 2.9 - 5.0, Look.w / 900.0), float3(2900.0, 2900.0, 96.0)) * 0.3;
float Dome = saturate((Patch - Layer.z) / 0.3);
float FairHeight = (Altitude - Layer.x) / Layer.w;
float FairCloud = 0.0;
if (FairHeight > 0.0)
{
	FairCloud = saturate(FairHeight * 12.0) * saturate((Dome - FairHeight) * 5.0);
}

// I bordi sfrangiati: un rumore fine che mangia la nuvola dove e' sottile.
float Shape = max(RainCloud, FairCloud);
float Detail = CloudNoise.Value(float3(FairX * 5.36, FairY * 5.36, Altitude / 280.0 + Look.w / 600.0), float3(5360.0, 5360.0, 144.0));
float Density = saturate(Shape * 1.25 - (1.0 - Shape) * Detail * 0.9 - 0.05);

// Il lampo dentro la nuvola: la nuvola attorno si accende.
float3 ToFlash = Metres - float3(Flash.x, Flash.y, Flash.z);
float Glow = Flash.w * Density * exp(-length(ToFlash) / 2500.0);

// Le nubi di pioggia sono piu' scure: piu' acqua, piu' estinzione.
return float4(Density * Look.x * (1.0 + 1.5 * Column.z), Glow, Density, Column.x);
)
