// =============================================================================
//  StreetLightShader.inl -- I lampioni nei materiali: i punti luce lontani
//  (M_GeoStreetLight) e il vetro dei pali (M_GeoStreetLamp). Nodi Custom.
//  HLSL E C++ INSIEME, come Weather/CloudShader.inl (le regole sono li'): la
//  factory dei materiali lo fa stringa, i test (georoads_main.cpp) lo
//  compilano come C++.
//
//  L'ACCENSIONE e' la stessa formula di StreetLights.h (LampOnLevel): la
//  soglia della fotocellula del lampione (un dato per istanza) per il
//  fattore della mattina, contro la luce che c'e' (MPC_GeoWeather
//  StreetLights.x). Cosi' il punto luce, il vetro del palo e la luce vera si
//  accendono insieme.
//
//  GEO_STREETLIGHT_SPRITE: lo spostamento dei vertici (World Position
//  Offset, float3, unita' di Unreal).
//    Ingressi: Pos (il vertice senza spostamenti, cm: e' la luce del
//    lampione, il quadrato e' grande 5 cm), Cam (la camera, cm), Corner (il
//    colore del vertice: x, y l'angolo 0/1), View (MPC StreetLightView: x
//    l'angolo di un pixel in radianti, y quanti pixel almeno, z il lato del
//    punto da vicino in m, w di quanti "punti" avvicinarlo alla camera),
//    UuPerMetre.
//  Il quadrato si apre verso la camera, largo View.z metri da vicino e mai
//  meno di View.y pixel da lontano: un lampione a 8 km e' ancora un puntino.
//  Si avvicina un poco alla camera (quanto un paio di "punti"): il terreno
//  lontano e' disegnato grossolano, e senza la luce ci finirebbe dentro.
//
//  GEO_STREETLIGHT_GLOW: il colore emesso del punto luce (float3).
//    Ingressi: Corner, Pos e Cam come sopra, Tint (i dati per istanza 0..2:
//    il colore della lampada per la sua luce), Switch (dato 3: la soglia
//    della fotocellula, lux), Lights (MPC StreetLights: x la luce che c'e',
//    lux; y il fattore della soglia, 1 la sera e 1,6 la mattina; z la luce al
//    piede di un lampione nelle unita' della scena; w la riduzione del flusso,
//    0 se spenti), View, UuPerMetre.
//  Un nucleo e un alone; da lontano, quando il punto e' piu' grande della
//  lampada, piu' debole (la luce si sparge su piu' pixel); da vicinissimo
//  sparisce: c'e' il vetro del palo.
//
//  GEO_STREETLIGHT_LAMP: il vetro dei pali (Emissive, float3).
//    Ingressi: Paint (il colore del vertice: w 1 sul vetro, StreetLights.h
//    BuildLampMesh), Tint, Switch, Lights.
// =============================================================================
GEO_STREETLIGHT_SPRITE(
float3 ToCam = Cam - Pos;
float Dist = max(length(ToCam), 1.0);
float3 Dir = ToCam * (1.0 / Dist);
float3 Helper = float3(0.0, 0.0, 1.0);
if (abs(Dir.z) > 0.95)
{
	Helper = float3(1.0, 0.0, 0.0);
}
float3 Right = normalize(float3(Helper.y * Dir.z - Helper.z * Dir.y, Helper.z * Dir.x - Helper.x * Dir.z, Helper.x * Dir.y - Helper.y * Dir.x));
float3 Up = float3(Dir.y * Right.z - Dir.z * Right.y, Dir.z * Right.x - Dir.x * Right.z, Dir.x * Right.y - Dir.y * Right.x);
float DistM = Dist / UuPerMetre;
float PixelM = DistM * View.x * View.y;
float SizeM = max(View.z, PixelM);
float PullM = min(View.w * PixelM, 0.3 * DistM);
float U = Corner.x - 0.5;
float V = Corner.y - 0.5;
return (Right * (U * SizeM) + Up * (V * SizeM) + Dir * PullM) * UuPerMetre;
)
GEO_STREETLIGHT_GLOW(
float U = (Corner.x - 0.5) * 2.0;
float V = (Corner.y - 0.5) * 2.0;
float R2 = U * U + V * V;
float Threshold = max(Switch * Lights.y, 0.001);
float On = saturate((Threshold * 1.15 - Lights.x) / (Threshold * 0.15));
float DistM = length(Cam - Pos) / UuPerMetre;
float SizeM = max(View.z, DistM * View.x * View.y);
float Far = exp(0.7 * log(View.z / SizeM));
float Near = saturate((DistM - 6.0) / 18.0);
float Shape = exp(-R2 * 10.0) + 0.08 * exp(-R2 * 3.0) * saturate(1.0 - R2);
return Tint * (On * Lights.w * Lights.z * 30.0 * Shape * Far * Near);
)
GEO_STREETLIGHT_LAMP(
float Threshold = max(Switch * Lights.y, 0.001);
float On = saturate((Threshold * 1.15 - Lights.x) / (Threshold * 0.15));
return Tint * (Paint.w * On * Lights.w * Lights.z * 30.0);
)
