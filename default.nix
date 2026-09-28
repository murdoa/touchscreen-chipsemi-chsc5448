{ config, lib, ... }:
let
  cfg = config.hardware.out-of-tree.touchscreen-chipsemi-chsc5448;
in
{
  options.hardware.out-of-tree.touchscreen-chipsemi-chsc5448.enable =
    lib.mkEnableOption "experimental out-of-tree Chipsemi CHSC5448 touchscreen driver";

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [
      (config.boot.kernelPackages.callPackage ./driver.nix { })
    ];
    boot.kernelModules = [ "chsc5448" ];
  };
}
