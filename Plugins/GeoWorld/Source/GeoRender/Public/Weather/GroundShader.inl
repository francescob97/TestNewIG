// =============================================================================
//  GroundShader.inl -- Il meteo sul suolo: il corpo del nodo Custom "Meteo sul
//  suolo" di M_GeoTerrain e M_GeoRoad. HLSL E C++ INSIEME, come
//  CloudShader.inl (leggi le regole li'): la factory dei materiali lo fa
//  stringa, i test (geoweather_main.cpp) lo compilano come C++.
//
//  Prende il colore e la ruvidita' di una superficie e li rende bagnati,
//  ghiacciati, coperti di grandine o innevati. Dal basso verso l'alto, cioe'
//  dall'ultimo che si vede al primo:
//
//    bagnato   piu' scuro (l'acqua riempie i pori) e lucido; non dove e' gelato
//    ghiaccio  un velo trasparente: scuro come il bagnato, appena azzurro,
//              lucidissimo. Sopra la sua quota (MPC GroundIce)
//    grandine  bianca, a chicchi da vicino. Dalla mappa al suolo (texture
//              GroundHail, posizione in MPC GroundHailMap: GroundHail.h)
//    neve      bianca sopra la copertura, sui pendii dolci, prima sui chiari
//
//  Ingressi: Colour, Rough, Normal (mondo), Pos (cm), Ground, Frame,
//  IceState, HailMap (MPC_GeoWeather), Hail + HailSampler (la mappa della
//  grandine), UuPerMetre, Depth (distanza dalla camera, cm).
//  Uscita: float4(colore, ruvidita').
//
//  LA QUOTA del pixel: il mondo di Unreal ha l'origine sull'ellissoide (il
//  rebase la tiene a quota zero), Z verso l'alto. Allontanandosi la Terra
//  scende sotto il piano: h = Z + d^2 / 2R (a 30 km, 70 m). Meno il geoide
//  all'origine: la quota sul mare, come la quota neve.
// =============================================================================
GEO_GROUND_SHADER(
float3 P = Pos * (1.0 / UuPerMetre);
float Altitude = P.z + (P.x * P.x + P.y * P.y) / (2.0 * Frame.y) - Frame.x;

// La neve resta sui pendii dolci, scivola via da quelli ripidi; sulla foto i
// chiari (prati, campi) si imbiancano prima dei boschi scuri.
float Hold = saturate((Normal.z - 0.55) / 0.3);
float Snow = saturate((Altitude - Ground.y) / max(Ground.z, 1.0) + 0.5) * Hold * Ground.w;
float Lum = Colour.x * 0.299 + Colour.y * 0.587 + Colour.z * 0.114;
Snow = saturate(Snow * (0.8 + 0.6 * Lum));

// Il ghiaccio: sopra la sua quota (lo zero termico, in ritardo), dove c'era
// acqua. Sotto la neve non si vede.
float Glaze = saturate((Altitude - IceState.y) / max(IceState.z, 1.0) + 0.5) * IceState.x * (1.0 - Snow);

// La grandine: dalla mappa al suolo. I chicchi rotolano via solo dai pendii
// piu' ripidi.
float HailU = (P.y - HailMap.y) * HailMap.w / HailMap.z + 0.5;
float HailV = (P.x - HailMap.x) / HailMap.z + 0.5;
float Stones = 0.0;
if (HailU > 0.0 && HailU < 1.0 && HailV > 0.0 && HailV < 1.0)
{
	Stones = Texture2DSampleLevel(Hail, HailSampler, float2(HailU, HailV), 0.0).x;
}
Stones = Stones * saturate((Normal.z - 0.35) / 0.3) * (1.0 - Snow);

// Da vicino si vedono i chicchi: celle di 3 cm, ognuna bianca se il suo numero
// a caso sta sotto la copertura. Da lontano (oltre ~15 m, dove una cella e'
// piu' piccola di un pixel e sfarfallerebbe) la copertura media.
uint GrainX = uint(int(floor(P.x * 33.0)));
uint GrainY = uint(int(floor(P.y * 33.0)));
uint GrainHash = (GrainX * 1597334677u) ^ (GrainY * 3812015801u);
GrainHash = GrainHash ^ (GrainHash >> 15u);
GrainHash = GrainHash * 2246822519u;
GrainHash = GrainHash ^ (GrainHash >> 13u);
float Grain = float(GrainHash) * 2.3283064e-10;
float Close = saturate(1.0 - Depth / UuPerMetre / 15.0);
Stones = lerp(Stones, saturate((Stones * 1.1 - Grain) * 8.0), Close);

float Wet = Ground.x * (1.0 - Snow) * (1.0 - Glaze);
float3 C = Colour * lerp(1.0, 0.55, Wet);
C = lerp(C, C * 0.7 + float3(0.05, 0.07, 0.10), Glaze);
C = lerp(C, float3(0.88, 0.90, 0.93), Stones);
C = lerp(C, float3(0.90, 0.92, 0.95), Snow);
float R = lerp(Rough, min(Rough, 0.18), Wet);
R = lerp(R, 0.05, Glaze);
R = lerp(R, 0.35, Stones);
R = lerp(R, 0.6, Snow);
return float4(C.x, C.y, C.z, R);
)
