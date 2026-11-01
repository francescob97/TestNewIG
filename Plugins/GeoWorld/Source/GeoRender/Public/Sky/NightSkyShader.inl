// =============================================================================
//  NightSkyShader.inl -- Il cielo di notte (M_GeoNightSky): stelle, Via
//  Lattea e la luna con le sue fasi. Un nodo Custom. HLSL E C++ INSIEME,
//  come Weather/CloudShader.inl (le regole sono li'): la factory dei
//  materiali lo fa stringa, i test (geosky_main.cpp) lo compilano come C++.
//
//  Si disegna su una sfera enorme attorno alla camera, additiva: di giorno
//  aggiunge quasi niente (solo la luna, pallida come si vede davvero), di
//  notte le stelle.
//
//  GEO_NIGHT_SKY: il colore emesso (float3).
//    Ingressi: View (CameraVector: dal punto verso la camera), EqX, EqY, EqZ
//    (MPC_GeoWeather SkyEquatorX/Y/Z: le righe della rotazione dal cielo
//    locale all'equatoriale, Astronomy.h HorizonToEquatorial), Moon
//    (SkyMoon: direzione e raggio in radianti), Sun (SkySun: direzione),
//    Night (SkyNight: x stelle 0..1, y il sole del livello in lux, w quanto
//    si vede la luna fra le nuvole), Catalog (SkyStars: x 2 se le stelle vere
//    del catalogo sono accese, Sky/StarShader.inl: allora queste no; 1 solo i
//    pianeti, e queste restano).
//
//  LE STELLE sono sul cielo EQUATORIALE: girano attorno alla stella polare un
//  giro al giorno siderale, come quelle vere. Non sono quelle vere (quelle del
//  catalogo, quando c'e', le disegna M_GeoStars e queste si spengono): una
//  griglia di celle di ~0,4 gradi, in una cella su ~28 una
//  stella, con la luminosita' che segue la statistica del cielo (tre volte
//  piu' stelle a ogni magnitudine in piu').
//  LA VIA LATTEA e' una fascia lungo il piano della galassia, piu' luminosa
//  verso il centro (nel Sagittario): quella si', nel posto giusto.
//  LA LUNA e' una sfera illuminata dal sole: per ogni punto del disco la
//  normale della sfera e il prodotto scalare con la direzione del sole. Le
//  fasi vengono da sole, falce compresa, girata nel verso giusto.
// =============================================================================
GEO_NIGHT_SKY(
float3 Dir = View * -1.0;
float3 E = float3(dot(Dir, float3(EqX.x, EqX.y, EqX.z)), dot(Dir, float3(EqY.x, EqY.y, EqY.z)), dot(Dir, float3(EqZ.x, EqZ.y, EqZ.z)));
float3 M = float3(Moon.x, Moon.y, Moon.z);
float3 S = float3(Sun.x, Sun.y, Sun.z);
float Lux = Night.y;
float3 Colour = float3(0.0, 0.0, 0.0);
if (Night.x > 0.001)
{
	float3 Cell = floor(E * 160.0);
	uint H = (uint)(int)Cell.x * 73856093u ^ (uint)(int)Cell.y * 19349663u ^ (uint)(int)Cell.z * 83492791u;
	H = (H ^ (H >> 16u)) * 0x7feb352du;
	H = (H ^ (H >> 15u)) * 0x846ca68bu;
	H = H ^ (H >> 16u);
	float Pick = (float)(H & 1023u) / 1024.0;
	if (Pick < 0.035 && Catalog.x < 1.5)
	{
		uint G = H * 0x9e3779b9u + 0x632be5abu;
		G = (G ^ (G >> 15u)) * 0x846ca68bu;
		G = G ^ (G >> 13u);
		float Jx = 0.15 + 0.7 * (float)(G & 255u) / 255.0;
		float Jy = 0.15 + 0.7 * (float)((G >> 8u) & 255u) / 255.0;
		float Jz = 0.15 + 0.7 * (float)((G >> 16u) & 255u) / 255.0;
		float Draw = 0.0005 + 0.999 * (float)((G >> 24u) & 255u) / 255.0;
		float3 Star = normalize(Cell + float3(Jx, Jy, Jz));
		float3 Gap = E - Star;
		float Magnitude = max(6.0 + log(Draw) / 1.0986, -1.5);
		float Flux = exp(-0.921 * Magnitude);
		float Profile = exp(-dot(Gap, Gap) / 4.2e-7);
		float Warm = (float)((H >> 10u) & 255u) / 255.0;
		float3 Tint = lerp(float3(0.72, 0.82, 1.0), float3(1.0, 0.86, 0.70), Warm);
		Colour = Colour + Tint * (Flux * Profile * 0.006 * Lux * Night.x);
	}
	float Band = dot(E, float3(-0.86767, -0.19808, 0.45599));
	float Plane = exp(-Band * Band / 0.0144);
	float Bulge = exp((dot(E, float3(-0.05487, -0.87341, -0.48386)) - 1.0) * 2.0);
	float Mottle = 0.6 + 0.4 * sin(E.x * 23.0 + sin(E.y * 17.0)) * sin(E.z * 19.0 + sin(E.x * 13.0));
	float MoonUp = saturate(M.z * 5.0 + 0.5);
	float Wash = 1.0 - 0.85 * (0.5 - 0.5 * dot(S, M)) * MoonUp;
	Colour = Colour + float3(0.86, 0.9, 1.0) * (Plane * (0.35 + 0.65 * Bulge) * Mottle * Wash * 2.5e-5 * Lux * Night.x);
}
float CosAngle = dot(Dir, M);
if (CosAngle > 0.99)
{
	float SinR = Moon.w;
	float3 Q = Dir - M * CosAngle;
	float R2 = dot(Q, Q) / (SinR * SinR);
	float Disc = saturate((1.0 - R2) * 10.0);
	float3 Normal = Q * (1.0 / SinR) - M * sqrt(saturate(1.0 - R2));
	float Lit = saturate(dot(Normal, S) * 3.0);
	float3 Face = float3(dot(Normal, float3(EqX.x, EqX.y, EqX.z)), dot(Normal, float3(EqY.x, EqY.y, EqY.z)), dot(Normal, float3(EqZ.x, EqZ.y, EqZ.z)));
	float Maria = saturate(0.5 + 0.5 * sin(Face.x * 5.0 + 1.3) * sin(Face.y * 4.0 + 0.4) + 0.3 * sin(Face.z * 9.0));
	float Albedo = 0.75 - 0.25 * Maria;
	float Fraction = 0.5 - 0.5 * dot(S, M);
	Colour = Colour + float3(1.0, 0.98, 0.94) * (Disc * Albedo * (Lit + 0.012) * 0.04 * Lux * Night.w);
	float Halo = exp(-sqrt(dot(Q, Q)) / 0.03) * Fraction * (1.0 - Disc);
	Colour = Colour + float3(0.8, 0.85, 1.0) * (Halo * 4.0e-4 * Lux * Night.x * Night.w);
}
return Colour;
)
