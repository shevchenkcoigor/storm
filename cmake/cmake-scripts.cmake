# Pinned by commit SHA, not tag: upstream deleted the 24.08.1 tag, so a cold CPM
# cache could no longer fetch it. 95f2126 is the commit that tag named (still on
# upstream main, so a full clone reaches it); VERSION keeps CPM's version
# bookkeeping as it was.
cpmaddpackage(
  NAME
  cmake-scripts
  GITHUB_REPOSITORY
  StableCoder/cmake-scripts
  VERSION
  24.08.1
  GIT_TAG
  95f2126620678779f172ccda6135e2f1d51f2220)
