#pragma once
#include <drogon/HttpController.h>

namespace aegis {

class CardController : public drogon::HttpController<CardController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(CardController::listTypes,  "/api/card-types",        drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::createType, "/api/card-types",        drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::deleteType, "/api/card-types/delete", drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::make,       "/api/cards/make",        drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::list,       "/api/cards",             drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::batch,      "/api/cards/batch",       drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::listBatches, "/api/card-batches",     drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::freeze,     "/api/cards/freeze",      drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::remove,     "/api/cards/delete",      drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(CardController::batchOp,    "/api/cards/batch-op",    drogon::Post, "aegis::AuthFilter");
    METHOD_LIST_END

    drogon::Task<> listTypes(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> createType(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> deleteType(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> make(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> list(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> batch(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> listBatches(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> freeze(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> remove(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> batchOp(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
};

} // namespace aegis
