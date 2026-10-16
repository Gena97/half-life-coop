"GameMenu"
{
	"1"
	{
		"label" "#GameUI_GameMenu_ResumeGame"
		"command" "ResumeGame"
		"OnlyInGame" "1"
	}
	"2"
	{
		"label" "New co-op campaign"
		"command" "engine exec newcampaign.cfg"
	}
	"3"
	{
		"label" "Выбрать главу или карту"
		"command" "OpenCreateMultiplayerGameDialog"
	}
	"4"
	{
		"label" "Save co-op game (F6)"
		"command" "engine coop_save quick"
		"OnlyInGame" "1"
		"notsingle" "1"
	}
	"5"
	{
		"label" "Continue co-op game"
		"command" "engine exec continuecoop.cfg"
	}
	"6"
	{
		"label" "Load co-op autosave"
		"command" "engine exec continueauto.cfg"
	}
	"7"
	{
		"label" "#GameUI_GameMenu_FindServers"
		"command" "OpenServerBrowser"
	}
	"8"
	{
		"label" "#GameUI_GameMenu_Options"
		"command" "OpenOptionsDialog"
	}
	"9"
	{
		"label" "#GameUI_GameMenu_SaveGame"
		"command" "OpenSaveGameDialog"
		"OnlyInGame" "1"
		"notmulti" "1"
	}
	"10"
	{
		"label" "#GameUI_GameMenu_LoadGame"
		"command" "OpenLoadGameDialog"
		"notmulti" "1"
	}
	"11"
	{
		"label" "#GameUI_GameMenu_Quit"
		"command" "Quit"
	}
}
