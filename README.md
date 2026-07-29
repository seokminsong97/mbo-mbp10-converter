# mbo-mbp10-converter

A deterministic, streaming MBO-to-MBP-10 reconstruction engine for DBN market
data. The converter is planned in C++, with an independent Python comparison
suite. The project is currently in the design phase; see [DESIGN.md](DESIGN.md).

## Important notice

This is an independent, unofficial open-source project. It is not affiliated
with, endorsed by, sponsored by, approved by, or supported by Databento, Inc.
The Databento name and related marks belong to their respective owners and are
used here only to describe format compatibility. Do not contact Databento for
support for this project.

This repository licenses source code only. It does not grant any license or
other rights to market data, DBN files obtained from a data provider, or data
owned by an exchange or another third party. Conversion into MBP-10 does not
remove restrictions from the source data or create redistribution rights.
Users are solely responsible for obtaining valid data access and complying
with all applicable provider terms, exchange licenses, laws, and regulations.
Do not commit or redistribute API keys or downloaded/derived market data unless
you are expressly authorized to do so.

The software and its output are provided **"AS IS"**, without warranties of
accuracy, completeness, merchantability, fitness for a particular purpose, or
non-infringement. Verify converted data independently before relying on it.
Neither this project nor its contributors provide investment, trading, legal,
or compliance advice, and they are not liable for trading losses, data loss, or
other damages arising from use of the software, to the extent permitted by
applicable law.

The source code is available under the [Apache License 2.0](LICENSE). If this
notice conflicts with the license as to the software, the license controls.
