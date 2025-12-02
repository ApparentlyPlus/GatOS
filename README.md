# GatOS: A Versatile, Modular Kernel for Toy-OS Builds

[![GatOS Linux](https://github.com/ApparentlyPlus/GatOS/actions/workflows/linux.yml/badge.svg)](https://github.com/ApparentlyPlus/GatOS/actions/workflows/linux.yml)
[![GatOS Windows](https://github.com/ApparentlyPlus/GatOS/actions/workflows/windows.yml/badge.svg)](https://github.com/ApparentlyPlus/GatOS/actions/workflows/windows.yml)
[![GatOS macOS](https://github.com/ApparentlyPlus/GatOS/actions/workflows/macOS.yml/badge.svg)](https://github.com/ApparentlyPlus/GatOS/actions/workflows/macOS.yml)
[![License: Custom](https://img.shields.io/badge/License-Custom-red.svg)](#license)

GatOS is a cleanly designed, modular kernel serving as the foundational layer for building toy operating systems. It is also part of my undergraduate thesis at the [University of Macedonia](https://www.uom.gr/en/dai), and serves as the backbone of a configurable, toy-OS building toolchain called PawStack.

> [!NOTE]
> This project is currently under heavy experimental development and is by no means ready for production use or general local deployment.

The first section of this README focuses on providing some insight as to the vision of this project. If you're just interested in running GatOS in your system, skip to the [Getting Started](#getting-started) section.


## Table of Contents

- [Project Overview & Background](#project-overview--background)
- [Getting Started](#getting-started)
- [Building the Toolchain from Source](#building-the-toolchain-from-source)
- [Testing](#testing)
- [Development](#development)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgments](#acknowledgments)


## Project Overview & Background

### What is PawStack?

"PawStack" is just the name I decided to use for a development toolchain that aims to drastically simplify OS development. It allows you to write code just like you would for a regular program — but instead of compiling to an application, your code is compiled directly into a complete, bootable operating system image.

This means your program ***is*** the operating system.

PawStack handles the complex parts of turning your code into low-level machine instructions that run on real hardware or emulators. The goal is to let you focus on building your OS's features without worrying about the usual technical challenges involved in OS development.

The whole toolchain is comprised of 3 components:

| Component | Description | Status |
|-----------|-------------|--------|
| **GatOS** | The current project. It is a modular kernel forming the core of PawStack. It aims to expose APIs and syscalls for core OS functionality. | **In Development** |
| **Gata** | A custom high-level programming language for writing operating systems. It will *feel* like a modern language but will be built with features that make low-level development simpler and more approachable. | **Planned** |
| **Appa** | The compiler for Gata. It takes in Gata source code and transpiles it into C code that calls GatOS's APIs. Appa constructs the kernel depending on the code's logic by leveraging the modularity of GatOS's design. The end result is a custom-configured version of GatOS for that specific Gata project. | **Planned** |

Technically, GatOS is not the end of the toolchain. Even after you have a version of GatOS generated for your Gata logic, the kernel itself still needs to be compiled into bare-metal machine code. This final build stage is handled by a GCC-based compilation toolchain, with packaging done through tools like grub-mkrescue and xorriso to produce a bootable image.

### Build Pipeline

```mermaid
graph LR
    A[Gata Source Code] --> B[Appa Compiler]
    B --> C[Custom GatOS Configuration]
    C --> D[GCC Toolchain]
    D --> E[Bootable OS Image]
```

> [!TIP]
> Currently, GatOS is the only project under development - with Gata and Appa to follow in a different Github repository once GatOS core is done.

> [!WARNING]
> It should also be noted that GatOS does not include its own bootloader, relying instead on GRUB for loading.

### What's with these names?

Glad you asked! Here's the story behind them:

**GatOS** is a playful pun on the Greek word *gatos* (meaning "male cat"), with the "OS" tacked on for "Operating System". It was inspired by a similar, more educationally focused project called [Skyl-OS](https://github.com/Billyzeim/Skyl-OS) — another pun, this time on *skylos* (meaning "male dog") — created by a close friend of mine. 

> [!TIP]
> If you're interested in learning OS development, I highly recommend checking out his work! His kernel is designed with teaching in mind (beginner to advanced concepts), while mine focuses more on optimizations, clean code, and modularity.

Following the same "cat" theme, I named the high-level language of the toolchain "**Gata**" — Greek for "female cat." It felt like the perfect fit for the language developers will use to interact with the toolchain, write code, and build their projects.

Finally, the compiler in the toolchain is called **Appa**. The name is inspired from the flying bison in Nickelodeon's animated series *"Avatar: The Last Airbender"*, a loyal companion to the main cast. The "bison" part is intentional — it's a direct nod to [GNU Bison](https://github.com/akimd/bison), the well-known syntax analysis tool used in building compilers.

"**PawStack**" is just a blend of comp-sci lingo and the animal based naming convention — perfect name for describing the entire toolchain ;)

### What is your university thesis on?

In short, my thesis focuses on developing a functional demo of the PawStack toolchain and thoroughly documenting its inner workings.

When I began, I had zero prior experience in OS development. Because of that, I see this as a great opportunity not only to deliver the demo, but also to create concise write-ups detailing my journey — what steps I took, the mistakes I made, what I omitted, what could be improved, and the features I implemented.

The end goal is for this to serve as a helpful reference in a field where accessible, beginner-friendly resources are scarce.

### Are you crazy?

Yes, absolutely. Name **one** other person who's trying to finish a 4-year degree in 3 years *while* building an entire operating system toolchain as their thesis.

This is either a feat of legendary ambition or an elaborate self-inflicted stress experiment. Possibly both.

## Getting Started

These instructions will get you a copy of the project up and running on your local machine for development and testing purposes.

> [!IMPORTANT]
> This project is a work in progress and building is not seamless yet. The project is not ready to be run locally in general, but if you're feeling adventurous and have the right setup, you can give it a shot.

### Prerequisites

Starting with `GatOS v1.7.5`, the build system's toolchain (GCC, Binutils, QEMU, GRUB, mtools, xorriso) has been statically cross-compiled for all major platforms. This enables a truly portable build system, allowing users to build the kernel on any mainstream operating system without installing dependencies.

### Supported Platforms

The following platforms are fully supported:

- **Linux** - Almost all distributions (including WSL) with Python 3.13+
- **Windows** - All versions that support Python 3.13+
- **macOS** - Both Intel and Apple Silicon (ARM) with Python 3.13+

### Quick Start

To build GatOS using the toolchain binaries:

1. Ensure Python 3.13+ is installed
2. Run the setup script to install and configure the toolchain (one-time setup)
3. Use the run script to build and execute the kernel

> [!NOTE]
> The prebuilt binaries are stable and will **NOT** be updated unless absolutely necessary.

#### Basic Usage:

```bash
# One time setup: Install and configure the toolchain
python setup.py

# Build and run the kernel in QEMU
python run.py

# That's it!
```

#### Build Options:

The `run.py` script supports several commands and options:
```bash
# Clean build artifacts
python run.py clean

# Build without running in QEMU
python run.py build

# Build (or build & run) an optimized (fast) image
python run.py [build] fast

# Build (or build & run) a highly optimized (very fast) image
python run.py [build] vfast

# Display all available commands and options
python run.py help
```

> [!NOTE]
> These scripts are intended for development purposes. The system is under active development, and features may change as the project evolves.


## Building the Toolchain from Source

> [!CAUTION]
> **This is strongly discouraged**, even for experienced developers.
> Attempt this only if you fully understand the scale of the undertaking.

While you *can try* to build the entire toolchain statically for your own platform if you do not wish to rely on the prebuilt binaries, please understand the following realities:

* The process is **extremely complex**, fragile, and heavily dependent on the host environment.
* It required **over 3 weeks** of nonstop trial and error to complete the provided build.
* Multiple components required **custom patching**, chaining patches on top of patches just to get them to compile.
* Keeping these builds working across platforms and versions would essentially require maintaining **an entirely separate project in its own right**.
* **No support will be provided** for source builds, because quite frankly it is outside the scope of the project.

Because I understand how difficult this process is, a small collection of experimental, incomplete, and largely unmaintained build scripts is included under `docs/toolchain/`. 

They:

* are **not guaranteed to work**,
* are **not tested**,
* can **break without warning**, and
* will almost certainly require **manual intervention** and fresh patching for newer upstream releases.

These scripts exist solely for transparency and educational insight, not as a supported or reliable build pipeline. For almost all users, including developers, using the prebuilt portable toolchain is the strongly recommended and intended workflow.

## Testing

As of GatOS version `1.7.5-alpha`, a test suite has been included in the kernel itself. It is built to be run in a live environment, which means, the kernel itself will run the tests if you instruct it to do so.

### Running Tests

To build a GatOS Test image, all you need to do is specify it in `run.py`:

```bash
python run.py build test
```

To run it (aka, to run the tests):

```bash
python run.py test
```

### Current Testing Approach

Currently, most of the tests are ran locally before deployment. It is not pretty, but it works while the core functionality is being developed. There are workflows that check the debug log output for the built-in tests as well.

If you want to automate the test process in a server (just like my Github runners), you can run a headless version of QEMU with a timeout, like so:

```python
python run.py test headless timeout=15s
```

The timeout is for the runner to stop QEMU after the specified time has elapsed. After that, you are free to write your own logic to parse `debug.log` and see if any tests have failed.

## Development

### Development Workflow

The development process follows a pretty standard Git workflow:

1. **Feature Branches**: New features are developed in separate branches
2. **Manual Testing**: Test your changes using the debug output approach
3. **CI Validation**: GitHub Actions runs automated checks on the QEMU serial output
4. **Merge**: Successfully tested branches get merged back to main

### Debugging

The main debugging tool is the `QEMU_LOG()` function. It's your best friend for figuring out what's happening (or not happening) in the kernel:

```c
QEMU_LOG("Kernel booting...", TOTAL_DBG);
// ... some code ...
QEMU_LOG("Memory manager initialized", TOTAL_DBG);
// ... more code ...
QEMU_LOG("Ready to handle interrupts", TOTAL_DBG);
```

You can also use all functions defined in `debug.h`, such as `QEMU_DUMP_PMT` for example, which dumps your page table structure in QEMU.

## Documentation

A lot of documentation and writeups are available in the [`docs/`](./docs/) folder, though this is not the focus of the project. This includes development notes, architecture decisions, learning resources, and basically everything I've figured out (or struggled with) during this journey.

Please note that the documentation is NOT always up to date. This is because new features (which are merged from new branches) are subject to change. It wouldn't be smart to update the documentation with every new release, if the next one will tweak things again.

For this reason, documentation gets updated after every 2-3 merges, when features have been solidifed into the kernel.

Again, a reminder that [Skyl-OS](https://github.com/Billyzeim/Skyl-OS) is a much better resource for beginners!

## Contributing

Contributions are not open since this is my thesis and thus must be my work alone. I need to be able to demonstrate that I understand every piece of code in this project, which means I have to write it myself.

However, you can still:
- **Report Issues**: If you find bugs or have questions, feel free to open issues
- **Provide Feedback**: Suggestions and feedback are always welcome through issues
- **Follow Along**: Watch the repository if you're interested in seeing how this progresses

Once the thesis is complete, I might consider opening it up for contributions, but that's a decision for future me.


## License

This project is licensed under a strict custom license that does not allow for replication of the code without explicit consent. I am unsure how this project will be used in the future, so the licensing is restrictive for now.

See the [LICENSE](LICENSE) file for details.

The restrictive nature is partly due to academic requirements and partly because I haven't decided what I want to do with this project long-term. This may change after thesis completion.

## Acknowledgments

- [Skyl-OS](https://github.com/Billyzeim/Skyl-OS) - A fantastic educational OS project from my dear friend, u/Billyzeim
- [The OS-Dev Wiki](https://wiki.osdev.org/Expanded_Main_Page) - The best starting place for OS development, with plenty of information on how to start.
- [MittOS64](https://github.com/thomasloven/mittos64) - Very good documentation that helped me through a lot of the struggles so far
- [Simple-OS](https://github.com/httpe/simple-os) - An already self-hosted modern kernel with libc ported, plenty useful for peeking inside implementations
- [OS-Series](https://github.com/davidcallanan/os-series/) - Helped me boostrap the entire project
- [OSDev-Notes](https://github.com/dreamportdev/Osdev-Notes/tree/master) - A book like no other, perfect for understanding every single detail of OS development

## Note to Readers

There’s a lot I want to build and many features I hope to implement in a very short time. Because of this, the README will likely evolve as the project progresses — with better documentation and clearer explanations of the project’s structure.  

For now, completing the project takes priority, so some parts remain unfinished or unexplained. Rest assured, these will be clarified once the initial rush is over. Thanks for your patience and interest!