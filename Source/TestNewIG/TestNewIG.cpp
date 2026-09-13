#include "TestNewIG.h"
#include "Modules/ModuleManager.h"

// IMPLEMENT_PRIMARY_GAME_MODULE genera il punto d'ingresso del modulo di gioco.
// Il terzo parametro e' il nome del modulo "primario": puo' essercene uno solo
// per progetto ed e' quello che da' il nome all'eseguibile.
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, TestNewIG, "TestNewIG");
