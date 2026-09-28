{
  lib,
  stdenv,
  kernel,
  kernelModuleMakeFlags,
}:

stdenv.mkDerivation {
  pname = "touchscreen-chipsemi-chsc5448";
  version = "${kernel.version}-0.1.0";
  src = ./src;

  nativeBuildInputs = kernel.moduleBuildDependencies;
  makeFlags = kernelModuleMakeFlags ++ [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "W=1"
  ];
  enableParallelBuilding = true;
  dontPatchELF = true;

  installPhase = ''
    runHook preInstall
    install -Dm444 chsc5448.ko \
      $out/lib/modules/${kernel.modDirVersion}/kernel/drivers/input/touchscreen/chsc5448.ko
    runHook postInstall
  '';

  meta = {
    description = "Experimental Chipsemi CHSC5448 touchscreen driver";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}
