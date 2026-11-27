// nrd_dump: writes the compute shader list of one NVIDIA NRD REBLUR_DIFFUSE denoiser (the denoiser of
// runtime/d3d9_nrd.mm). For each pipeline of the NRD instance, the tool writes one line to standard output:
//
//   <index> <shader identifier>
//
// The shader identifier has the NRD format "fileName|macro1=value1|macro2=value2...". tools/nrd_msl.py compiles each
// shader from its identifier.
//
//   nrd_dump
#include <NRD.h>

#include <cstdint>
#include <cstdio>

static int fail(const char* what) {
    std::fprintf(stderr, "nrd_dump: %s\n", what);
    return 1;
}

int main(int argc, char**) {
    if (argc != 1)
        return fail("usage: nrd_dump");
    const nrd::LibraryDesc* lib = nrd::GetLibraryDesc();
    if (!lib || lib->versionMajor != NRD_VERSION_MAJOR || lib->versionMinor != NRD_VERSION_MINOR)
        return fail("the NRD library and its headers have different versions");

    const nrd::DenoiserDesc denoiser = {0, nrd::Denoiser::REBLUR_DIFFUSE};
    nrd::InstanceCreationDesc cd = {};
    cd.denoisers = &denoiser;
    cd.denoisersNum = 1;
    nrd::Instance* inst = nullptr;
    if (nrd::CreateInstance(cd, inst) != nrd::Result::SUCCESS || !inst)
        return fail("nrd::CreateInstance (REBLUR_DIFFUSE) failed");
    const nrd::InstanceDesc* d = nrd::GetInstanceDesc(*inst);

    int rc = 0;
    for (uint32_t i = 0; i < d->pipelinesNum && rc == 0; i++) {
        const char* id = d->pipelines[i].shaderIdentifier;
        if (id[0] == '\0' || id[0] == '|')
            rc = fail("a pipeline has no shader identifier");
        else if (std::printf("%u %s\n", i, id) < 0)
            rc = fail("cannot write the pipeline list");
    }
    if (rc == 0 && std::fflush(stdout) != 0)
        rc = fail("cannot write the pipeline list");
    nrd::DestroyInstance(*inst);
    return rc;
}
