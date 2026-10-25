@echo off
rem Lance le jeu sous RenderDoc (écran TV + GamePad). F12 pendant la partie : capture de l'image
rem suivante dans recomp\rdoc\ (environ 200 Mo par capture). Arguments supplémentaires transmis au jeu.
set ENABLE_VULKAN_RENDERDOC_CAPTURE=1
set VK_LAYER_PATH=%~dp0..\tools\renderdoc_dl\RenderDoc_1.46_64
set VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture
"%~dp0build\pikmin3.exe" --split %*
