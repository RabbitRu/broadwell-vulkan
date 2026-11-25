Measurement build of 25.11.2026 (not part of main; the switches are read from the environment)

  git checkout b12d90e          (main: "Colour fast clears (CCS_D) for single-level 2D images")
  git apply -p2 <this folder>/measurement.patch
  tools\build-icd.ps1, then copy build\icd elsewhere and run with -Icd <copy>\intel_hasvk_wddm_icd.json

HASVK_WDDM_PROFILE_PASSES=1   GPU time of every render pass (vkCmdBeginRenderPass and vkCmdBeginRendering),
                              dispatch and transfer, with pipeline statistics; a table every 5 s on stderr
HASVK_WDDM_PROFILE_DRAWS=1    also every draw, by pipeline
HASVK_WDDM_PROFILE_BARRIERS=1 also every vkCmdPipelineBarrier(2), by the first image's size and layouts
HASVK_EXP_FRAMELOG=<n>[:<k>]  (with PROFILE_PASSES) every pass, barrier, transfer and submission recorded while
                              presents n..n+k-1 are current, with image pointers: framelog2.py <stdout> <n>
HASVK_EXP_CLAMP=<width>       render areas at least that wide shrunk to 16x16 (wrong picture): what those
                              passes cost the frame. 2048 = window-size passes at 2560x1600, 1 = every pass
HASVK_EXP_MOCS_RT=<hex>[,<MiB>]  MOCS of render targets, depth and stencil of at least that size
HASVK_EXP_MOCS=<hex>          MOCS of everything the driver owns (Mesa: 78, Intel's driver: 79)
HASVK_EXP_HIZ1=1              depth fast clears to 1.0 for every image (sampled depth then reads 0.0)
HASVK_EXP_CCS=0               CCS_D off
HASVK_EXP_CCS_NOSKIP=1        every layout transition records its predicated CCS resolve

Scripts: agg_vk.py (pass table over the timedemo windows), agg_draws.py (draws by pipeline),
framelog2.py (per-image timeline of a logged frame), exp-bound.ps1 (interleaved Dota series by variant),
surf_state.py (decodes RENDER_SURFACE_STATE candidates found by D3D11_PROBE_FIND).
