// =============================================================================
//  BuildingShader.inl -- Il materiale degli edifici (M_GeoBuilding): un nodo
//  Custom. HLSL E C++ INSIEME, come Weather/CloudShader.inl e
//  Vegetation/VegetationShader.inl (le regole sono li'): la factory dei
//  materiali lo fa stringa, i test (geobuildings_main.cpp) lo compilano come
//  C++.
//
//  GEO_BUILDING_COLOUR: il colore (Base Color) e la ruvidita'.
//    Ingressi: Paint (il colore del vertice: RGB il colore in sRGB, A 1 muro /
//    0 tetto: BuildingMesh.h), Facade (UV0: metri lungo il muro, metri sopra
//    la base), Style (UV1: altezza di un piano, 0 = niente finestre; un caso
//    0..1 dell'edificio), Normal (mondo), Pos (cm), Ground e Frame
//    (MPC_GeoWeather: bagnato, neve, quota), UuPerMetre.
//
//  LE FINESTRE non sono triangoli: la facciata si divide in campate (3,2-4 m,
//  secondo il caso dell'edificio) e in piani; in ogni casella una finestra,
//  meta' larga e meta' alta, vetro scuro e lucido. Sotto il suolo niente.
//  Una riga piu' scura fra un piano e l'altro (il marcapiano).
//  LA NEVE sui tetti (e su quello che guarda il cielo) sopra la quota della
//  copertura del suolo, come per il terreno e gli alberi; IL BAGNATO scurisce
//  i tetti piu' dei muri (la pioggia batte dall'alto).
//  Il colore arriva in sRGB: al quadrato e' (quasi) lineare.
//
//  GEO_BUILDING_LIGHTS: le finestre accese di notte (Emissive, float3).
//    Ingressi: Paint, Facade, Style e Normal come sopra, Night
//    (MPC_GeoWeather SkyNight: y il sole del livello in lux, z quante
//    finestre sono accese 0..1, dall'ora e dal buio: Sky/DayLight.h).
//  Ogni finestra ha il suo numero a caso (la campata, il piano, il muro,
//  l'edificio): e' accesa se il numero sta sotto la quota delle accese.
//  Quando la quota cala (dopo mezzanotte) si spengono sempre le stesse, una
//  alla volta. Luce calda di lampadina, qualcuna fredda (LED, televisori);
//  ~1/250 del sole del livello: 30 volte un muro sotto la luna.
// =============================================================================
GEO_BUILDING_COLOUR(
float Wall = Paint.w;
float3 C = float3(Paint.x * Paint.x, Paint.y * Paint.y, Paint.z * Paint.z);
float R = lerp(0.85, 0.9, Wall);
if (Wall > 0.5 && Style.x > 0.5)
{
	float Bay = 3.2 + 0.8 * Style.y;
	float U = Facade.x / Bay;
	float Across = U - floor(U);
	float V = Facade.y / Style.x;
	float Storey = floor(V);
	float Up = V - Storey;
	float FromMiddle = Across - 0.5;
	float FromSill = Up - 0.56;
	float InX = saturate((0.22 - abs(FromMiddle)) * 40.0);
	float InY = saturate((0.24 - abs(FromSill)) * 40.0);
	float Window = InX * InY * saturate(Storey + 1.0);
	C = lerp(C, float3(0.045, 0.05, 0.06), Window);
	R = lerp(R, 0.15, Window);
	float Course = saturate((0.04 - Up) * 60.0) * saturate(Storey);
	C = C * (1.0 - 0.18 * Course);
}
float3 P = Pos * (1.0 / UuPerMetre);
float Altitude = P.z + (P.x * P.x + P.y * P.y) / (2.0 * Frame.y) - Frame.x;
float Cover = saturate((Altitude - Ground.y) / max(Ground.z, 1.0) + 0.5) * Ground.w;
float Snow = Cover * saturate((Normal.z - 0.3) / 0.4);
C = C * (1.0 - 0.35 * Ground.x * (1.0 - 0.5 * Wall));
R = lerp(R, 0.35, Ground.x * (1.0 - Wall));
C = lerp(C, float3(0.82, 0.85, 0.90), Snow * 0.9);
R = lerp(R, 0.7, Snow);
return float4(C.x, C.y, C.z, R);
)
GEO_BUILDING_LIGHTS(
float3 Glow = float3(0.0, 0.0, 0.0);
if (Paint.w > 0.5 && Style.x > 0.5 && Night.z > 0.001)
{
	float Bay = 3.2 + 0.8 * Style.y;
	float U = Facade.x / Bay;
	float Column = floor(U);
	float V = Facade.y / Style.x;
	float Storey = floor(V);
	float FromMiddle = U - Column - 0.5;
	float FromSill = V - Storey - 0.56;
	float InX = saturate((0.22 - abs(FromMiddle)) * 40.0);
	float InY = saturate((0.24 - abs(FromSill)) * 40.0);
	float Window = InX * InY * saturate(Storey + 1.0);
	uint Wall = (uint)(int)floor(Normal.x * 4.0 + 4.5) * 9u + (uint)(int)floor(Normal.y * 4.0 + 4.5);
	uint H = (uint)(int)Column * 73856093u ^ (uint)(int)Storey * 19349663u ^ (uint)(Style.y * 65535.0) * 83492791u ^ Wall * 2654435761u;
	H = (H ^ (H >> 16u)) * 0x7feb352du;
	H = (H ^ (H >> 15u)) * 0x846ca68bu;
	H = H ^ (H >> 16u);
	float Pick = (float)(H & 1023u) / 1024.0;
	float Lit = saturate((Night.z - Pick) * 30.0);
	float Tone = (float)((H >> 10u) & 255u) / 255.0;
	float3 Lamp = lerp(float3(1.0, 0.72, 0.42), float3(0.75, 0.85, 1.0), Tone * Tone * Tone);
	float Level = 0.6 + 0.8 * (float)((H >> 18u) & 255u) / 255.0;
	Glow = Lamp * (Window * Lit * Level * 0.004 * Night.y);
}
return Glow;
)
