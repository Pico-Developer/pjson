//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License").
//
//===----------------------------------------------------------------------===//
// 07 — A small address-book application (capstone)
//
// Ties everything together: a schema for a contact, building records, parsing
// an incoming payload, validating it, editing the store, and serializing the
// result. Referenced by docs/07-capstone-address-book.md.
//
#include "pjson.h"

#include <climits>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace ByteDance;

namespace {

    // Builds the schema every contact must satisfy. The embedded literal is
    // fixed application data, so parsing it is expected to succeed.
    pjson::unique_ptr contactSchema() {
        return pjson::parse(R"({
        "type": "object",
        "required": ["id", "name", "emails"],
        "properties": {
            "id":     { "type": "integer", "minimum": 1 },
            "name":   { "type": "string", "minLength": 1 },
            "emails": { "type": "array", "minItems": 1, "items": { "type": "string", "pattern": "@" } },
            "tags":   { "type": "array", "items": { "type": "string" } }
        }
    })");
    }

    // Adds a deep copy of a valid contact to the book. Invalid contacts leave
    // the book unchanged and produce one line for every validation failure.
    bool addContact(pjson& book, const pjson& schema, const pjson& contact) {
        std::vector<pjson::SchemaError> errors;
        if (!contact.validate(schema, errors)) {
            std::cout << "  rejected contact:\n";
            for (const pjson::SchemaError& e : errors) {
                std::cout << "    " << (e.path.empty() ? "(root)" : e.path) << ": " << e.message
                          << "\n";
            }
            return false;
        }
        // Append by assigning to the next array index (there is no operator+= for a
        // whole pjson value; indexing auto-extends the array, then we copy in).
        pjson& contacts = book["contacts"];
        if (contacts.size() > static_cast<size_t>(INT_MAX))
            return false;
        contacts[static_cast<int>(contacts.size())] = contact;
        return true;
    }

} // namespace

// Runs the address-book workflow: initialize, ingest, reject, edit, and query.
int main() {
    // --- Initialize the store ---------------------------------------------
    pjson::unique_ptr schema = contactSchema();
    if (!schema) {
        std::cerr << "could not parse the embedded schema\n";
        return 1;
    }

    // Start an empty address book.
    pjson book;
    book["version"] = int64_t(1);
    book["contacts"].resetTo(pjson::jsonArray); // start as an empty array

    // --- Ingest contacts --------------------------------------------------
    // 1) Build a contact programmatically.
    pjson ada;
    ada["id"] = int64_t(1);
    ada["name"] = "Ada Lovelace";
    ada["emails"] += "ada@example.com";
    ada["tags"] += "pioneer";
    std::cout << "adding Ada...\n";
    addContact(book, *schema, ada);

    // 2) Accept a contact that arrives as a JSON payload.
    std::cout << "adding incoming payload...\n";
    auto incoming = pjson::parse(R"({
        "id": 2, "name": "Bob", "emails": ["bob@example.com", "b@work.com"]
    })");
    if (!incoming) {
        std::cerr << "could not parse incoming contact\n";
        return 1;
    }
    addContact(book, *schema, *incoming);

    // 3) Reject an invalid contact.
    std::cout << "adding invalid contact...\n";
    auto invalid = pjson::parse(R"({ "id": 0, "name": "", "emails": [] })");
    if (!invalid) {
        std::cerr << "could not parse invalid-contact fixture\n";
        return 1;
    }
    addContact(book, *schema, *invalid);

    // --- Edit and query ---------------------------------------------------
    // 4) Edit the store: give Ada a second email, then look someone up.
    book["contacts"][0]["emails"] += "ada@lovelace.org";

    std::cout << "\nlookup id=2: ";
    const pjson* contacts = book.find("contacts");
    for (size_t i = 0; contacts && i < contacts->size(); ++i) {
        const pjson* contact = contacts->find(static_cast<int>(i));
        int64_t id = 0;
        std::string name;
        if (contact && contact->tryGet("id", id) && id == int64_t(2) &&
            contact->tryGet("name", name))
            std::cout << name << "\n";
    }

    // --- Serialize --------------------------------------------------------
    // 5) Serialize the whole book.
    pjson::SerializeOptions output = pjson::SerializeOptions::prettyPrinted();
    output.maxOutputBytes = size_t(64) * 1024 * 1024;
    std::cout << "\nfinal address book:\n" << book.toString(output) << "\n";
    return 0;
}
