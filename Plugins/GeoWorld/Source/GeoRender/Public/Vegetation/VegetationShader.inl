// =============================================================================
//  VegetationShader.inl -- Il materiale degli alberi (M_GeoVegetation): due
//  nodi Custom. HLSL E C++ INSIEME, come Weather/CloudShader.inl (le regole
//  sono li'): la factory dei materiali li fa stringa, i test
//  (geovegetation_main.cpp) li compilano come C++.
//
//  Due blocchi, due macro: chi include definisce quella che gli serve e lascia
//  vuota l'altra.
//
//  GEO_VEGETATION_COLOUR: il colore (Base Color) e la ruvidita'.
//    Ingressi: Leaf (il verde dell'istanza, dai suoi dati: TreePlacement.h),
//    Paint (il colore del vertice: R chioma/tronco, G luce del cielo, B
//    altezza nell'albero: TreeMesh.h), Normal (mondo), Pos (cm), Ground e
//    Frame (MPC_GeoWeather: la neve e la quota), UuPerMetre.
//    La neve: sopra la quota della copertura del suolo, sulla chioma che
//    guarda il cielo; i tronchi restano scuri.
//
//  GEO_VEGETATION_WIND: lo spostamento dei vertici (World Position Offset).
//    Ingressi: Paint, Wind (MPC_GeoWeather VegetationWind: il vento a 10 m
//    verso nord e verso est in m/s, la raffica 0..1, l'orologio in s), Size
//    (l'altezza dell'albero, m), Phase (un numero a caso 0..1 dell'istanza),
//    UuPerMetre. Uscita: lo spostamento in cm, negli assi del mondo.
//    L'albero si piega sottovento tanto piu' quanto piu' e' in alto (la
//    base non si muove mai) e quanto piu' forte e' il vento (col quadrato),
//    e oscilla con un suo periodo di 2-4 secondi.
// =============================================================================
GEO_VEGETATION_COLOUR(
float Foliage = Paint.x;
float Light = Paint.y;
float3 Bark = float3(0.10, 0.075, 0.055);
float3 C = lerp(Bark, Leaf, Foliage) * (0.35 + 0.65 * Light);

float3 P = Pos * (1.0 / UuPerMetre);
float Altitude = P.z + (P.x * P.x + P.y * P.y) / (2.0 * Frame.y) - Frame.x;
float Cover = saturate((Altitude - Ground.y) / max(Ground.z, 1.0) + 0.5) * Ground.w;
float Snow = Cover * saturate((Normal.z - 0.15) / 0.5) * Foliage;
C = lerp(C, float3(0.82, 0.85, 0.90), Snow * 0.9);
float R = lerp(0.85, 0.72, Foliage);
return float4(C.x, C.y, C.z, R);
)
GEO_VEGETATION_WIND(
float Height = Paint.z;
float Speed = sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
float DownNorth = 0.0;
float DownEast = 0.0;
if (Speed > 0.01)
{
	DownNorth = Wind.x / Speed;
	DownEast = Wind.y / Speed;
}
float Bend = 0.00025 * Size * Speed * Speed * (1.0 + 0.6 * Wind.z);
float Period = 2.0 + 2.0 * Phase;
float Sway = sin(6.2831853 * (Wind.w / Period + Phase));
float Reach = Bend * (1.0 + 0.35 * Sway) * Height * Height;
return float3(DownNorth * Reach * UuPerMetre, DownEast * Reach * UuPerMetre, 0.0);
)
