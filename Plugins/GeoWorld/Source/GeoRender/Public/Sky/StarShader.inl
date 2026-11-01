// =============================================================================
//  StarShader.inl -- Le stelle vere e i pianeti (M_GeoStars). Nodi Custom.
//  HLSL E C++ INSIEME, come Weather/CloudShader.inl (le regole sono li'): la
//  factory dei materiali lo fa stringa, i test (geosky_main.cpp) lo compilano
//  come C++.
//
//  Ogni stella e' un quadratino di 5 cm in un componente di istanze attorno
//  alla camera, a 150 km (Sky/Stars.h): il materiale lo apre verso la camera,
//  largo qualche pixel, come i punti luce dei lampioni.
//
//  GEO_STAR_SPRITE: lo spostamento dei vertici (World Position Offset, cm).
//    Ingressi: Pos (il vertice senza spostamenti, cm), Cam (la camera, cm),
//    Corner (il colore del vertice: x, y l'angolo 0/1), Stars (MPC
//    SkyStars: x 2 le stelle del catalogo e i pianeti, 1 solo i pianeti, 0
//    niente; y la magnitudine limite, z quanta luce,
//    w l'angolo di un pixel in radianti), Mag (il dato per istanza 3: la
//    magnitudine), UuPerMetre.
//  Piu' luminosa la stella, piu' grande il punto: da 1,5 pixel (le piu'
//  deboli che si vedono) a 4,4 (una stella di magnitudine 0), 6 Venere, mai
//  piu' di 7. E' quello che fa l'occhio (e una
//  fotografia): una stella non ha una dimensione, ma la sua luce si sparge.
//
//  GEO_STAR_GLOW: il colore emesso (float3).
//    Ingressi: Corner, Pos e Cam come sopra, Tint (dati 0..2: il colore),
//    Mag (dato 3), Seed (dato 4: la fase della scintillazione; < 0 un
//    pianeta, che non scintilla), Stars, Clock (il tempo, s).
//  La luce e' quella della magnitudine (2,512 volte per ogni magnitudine in
//  meno), sotto la magnitudine limite sfuma in una magnitudine; vicino
//  all'orizzonte l'aria la attenua (0,25 magnitudini per massa d'aria) e la
//  fa scintillare; sotto l'orizzonte niente.
// =============================================================================
GEO_STAR_SPRITE(
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
float Pixels = min(max(2.0 + (Stars.y - Mag) * 0.4, 1.5), 7.0);
float SizeM = Dist / UuPerMetre * Stars.w * Pixels;
float U = Corner.x - 0.5;
float V = Corner.y - 0.5;
return (Right * (U * SizeM) + Up * (V * SizeM)) * UuPerMetre;
)
GEO_STAR_GLOW(
float U = (Corner.x - 0.5) * 2.0;
float V = (Corner.y - 0.5) * 2.0;
float R2 = U * U + V * V;
float3 Look = normalize(Pos - Cam);
float SinH = Look.z;
float Below = saturate(SinH * 40.0 + 1.0);
float H = max(SinH, 0.0);
float Airmass = 1.0 / (H + 0.025 * exp(-11.0 * H));
float Extinction = exp(-0.23 * (Airmass - 1.0));
float Visible = saturate(Stars.y - Mag + 0.5);
float Twinkle = 1.0;
if (Seed >= 0.0)
{
	Twinkle = 1.0 + 0.35 * (1.0 - saturate(SinH * 2.0)) * sin(Clock * (9.0 + 7.0 * Seed) + Seed * 40.0);
}
float Flux = exp(-0.921 * Mag);
float Shape = exp(-R2 * 5.0) * saturate(1.0 - R2);
return Tint * (saturate(Stars.x) * Stars.z * Flux * Visible * Extinction * Below * Twinkle * Shape);
)
