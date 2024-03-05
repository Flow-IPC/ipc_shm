# Flow-IPC Sub-project -- Shared Memory -- End-to-end zero-copy transport support; direct work with SHM; SHM-classic provider

This project is a sub-project of the larger Flow-IPC meta-project.  Please see
a similar `README.md` for Flow-IPC, first.  You can most likely find it either in the parent
directory to this one; or else in a sibling GitHub repository named `ipc.git`.

A more grounded description of the various sub-projects of Flow-IPC, including this one, can be found
in `./src/ipc/common.hpp` off the directory containing the present README.  Look for
`Distributed sub-components (libraries)` in a large C++ comment.

Took a look at those?  Still interested in `ipc_shm` as an independent entity?  Then read on.
Before you do though: it is, typically, both easier and more functional to simply treat Flow-IPC as a whole.
To do so it is sufficient to never have to delve into topics discussed in this README.  In particular
the Flow-IPC generated documentation guided Manual + Reference are monolithic and cover all the
sub-projects together, including this one.

Still interested?  Then read on.

`ipc_shm` depends on `ipc_session` (and all its dependencies; i.e. `ipc_transport_structured`, `ipc_core`, `flow`).
It provides `ipc::shm::stl`, `ipc::transport::struc::shm` (including `ipc::transport::struc::shm::classic` but
excluding all other such sub-namespaces), `ipc::shm::classic`, and `ipc::session::shm::classic`.

Roughly speaking one can think of `ipc_shm` in 3 ways, from the developer's point of view.  These are ordered
in descending order of mass appeal, by our estimation.

  - It **empowers the established concepts of its dependencies** (`ipc::transport` and
    `ipc::session`) **to gain end-to-end-zero-copy performance and semantics**.  Thus, by adding a few characters
    to a template name used in your code, your sessions and channels will now entirely refrain from copying
    any of your data.  Internally this is done by using SHared Memory (SHM), but you don't have to worry about
    details: just know that (1) no copying is occurring; and (2) modifying a thing "here" has effect on that thing
    earlier having been received "there."
    - This is of interest for, we estimate, most developers interested in IPC.
  - It adds the ability to transmit (again, with zero-copy) **native C++ data structures**
    (of arbitrary complexity, including STL containers of STL containers of ... of `struct`s of STL containers of...
    etc.).  All the hairy goodies -- allocators, fancy-pointers, and so on -- are provided.  As a result you
    get to simply place a C++ data structure in memory, transmit a handle to it over an IPC channel,
    and on the other side have access to the very same C++ data structure, at the same location in physical
    RAM.
    - This is of interest as an advanced feature for a healthy number of devs interested in IPC, we estimate.
      One tries to keep it to the capnp-backed schema-based messages of `ipc::transport::struc`; but
      sometimes that's not sufficient, and one will want to "go native with it."
  - At the lowest level, direct access to SHM in a classic way is provided as well.  (Internally this backs
    the higher-level SHM-enabled features above.)  This is called the **SHM-classic SHM provider**.
    Typically one need not worry about it, other than specifying that this is SHM-provider one would like
    to use for their entire session.  However, if desired, the SHM-classic pools can be set up and accessed
    directly.  In that sense it is akin to boost.interprocess's delightful SHM support.

## Documentation

See Flow-IPC meta-project's `README.md` Documentation section.  `ipc_shm` lacks its own generated documentation.
However, it contributes to the aforementioned monolithic documentation through its many comments which can
(of course) be found directly in its code (`./src/ipc/...`).  (The monolithic generated documentation scans
these comments using Doxygen, combined with its siblings' comments... and so on.)

## Obtaining the source code

- As a tarball/zip: The [project web site](https://flow-ipc.github.io) links to individual releases with notes, docs,
  download links.  We are included in a subdirectory off the Flow-IPC root.
- Via Git:
  - `git clone --recurse-submodules git@github.com:Flow-IPC/ipc.git`; or
  - `git clone git@github.com:Flow-IPC/ipc_shm.git`

## Installation

See [INSTALL](./INSTALL.md) guide.

## Contributing

See [CONTRIBUTING](./CONTRIBUTING.md) guide.
