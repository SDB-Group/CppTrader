/*!
    \file market_manager.cpp
    \brief Market manager implementation
    \author Ivan Shynkarenka
    \date 03.08.2017
    \copyright MIT License
*/

#include "trader/matching/market_manager.h"

namespace CppTrader {
namespace Matching {

MarketHandler MarketManager::_default;

MarketManager::~MarketManager()
{
    // Release orders
    for (auto& order : _orders)
        _order_pool.Release(order.second);
    _orders.clear();

    // Release order books
    for (auto order_book_ptr : _order_books)
        if (order_book_ptr != nullptr)
            _order_book_pool.Release(order_book_ptr);
    _order_books.clear();

    // Release symbols
    for (auto symbol_ptr : _symbols)
        if (symbol_ptr != nullptr)
            _symbol_pool.Release(symbol_ptr);
    _symbols.clear();
}

ErrorCode MarketManager::AddSymbol(const Symbol& symbol)
{
    // Resize the symbol container
    if (_symbols.size() <= symbol.Id)
        _symbols.resize(symbol.Id + 1, nullptr);

    // Create a new symbol
    Symbol* symbol_ptr = _symbol_pool.Create(symbol);

    // Insert the symbol
    assert((_symbols[symbol.Id] == nullptr) && "Duplicate symbol detected!");
    if (_symbols[symbol.Id] != nullptr)
    {
        // Release the symbol
        _symbol_pool.Release(symbol_ptr);
        return ErrorCode::SYMBOL_DUPLICATE;
    }
    _symbols[symbol.Id] = symbol_ptr;

    // Call the corresponding handler
    _market_handler.onAddSymbol(*symbol_ptr);

    return ErrorCode::OK;
}

ErrorCode MarketManager::DeleteSymbol(uint32_t id)
{
    assert(((id < _symbols.size()) && (_symbols[id] != nullptr)) && "Symbol not found!");
    if ((_symbols.size() <= id) || (_symbols[id] == nullptr))
        return ErrorCode::SYMBOL_NOT_FOUND;

    // Get the symbol by Id
    Symbol* symbol_ptr = _symbols[id];

    // Call the corresponding handler
    _market_handler.onDeleteSymbol(*symbol_ptr);

    // Erase the symbol
    _symbols[id] = nullptr;

    // Release the symbol
    _symbol_pool.Release(symbol_ptr);

    return ErrorCode::OK;
}

ErrorCode MarketManager::AddOrderBook(const Symbol& symbol)
{
    assert(((symbol.Id < _symbols.size()) && (_symbols[symbol.Id] != nullptr)) && "Symbol not found!");
    if ((_symbols.size() <= symbol.Id) || (_symbols[symbol.Id] == nullptr))
        return ErrorCode::SYMBOL_NOT_FOUND;

    // Get the symbol by Id
    Symbol* symbol_ptr = _symbols[symbol.Id];

    // Resize the order book container
    if (_order_books.size() <= symbol.Id)
        _order_books.resize(symbol.Id + 1, nullptr);

    // Create a new order book
    OrderBook* order_book_ptr = _order_book_pool.Create(*symbol_ptr);

    // Insert the order book
    assert((_order_books[symbol.Id] == nullptr) && "Duplicate order book detected!");
    if (_order_books[symbol.Id] != nullptr)
    {
        // Release the order book
        _order_book_pool.Release(order_book_ptr);
        return ErrorCode::ORDER_BOOK_DUPLICATE;
    }
    _order_books[symbol.Id] = order_book_ptr;

    // Call the corresponding handler
    _market_handler.onAddOrderBook(*order_book_ptr);

    return ErrorCode::OK;
}

ErrorCode MarketManager::DeleteOrderBook(uint32_t id)
{
    assert(((id < _order_books.size()) && (_order_books[id] != nullptr)) && "Order book not found!");
    if ((_order_books.size() <= id) || (_order_books[id] == nullptr))
        return ErrorCode::ORDER_BOOK_NOT_FOUND;

    // Get the order book by Id
    OrderBook* order_book_ptr = _order_books[id];

    // Call the corresponding handler
    _market_handler.onDeleteOrderBook(*order_book_ptr);

    // Erase the order book
    _order_books[id] = nullptr;

    // Release the order book
    _order_book_pool.Release(order_book_ptr);

    return ErrorCode::OK;
}

ErrorCode MarketManager::AddOrder(const Order& order)
{
    // Add the corresponding order type
    switch (order.Type)
    {
        case OrderType::MARKET:
            return AddMarketOrder(order);
        case OrderType::LIMIT:
            return AddLimitOrder(order);
        default:
            return ErrorCode::ORDER_TYPE_INVALID;
    }
}

ErrorCode MarketManager::AddMarketOrder(const Order& order)
{
    assert(_matching && "Market orders can be placed only in automatic matching mode!");

    // Validate parameters
    assert((order.Id > 0) && "Order Id must be greater than zero!");
    if (order.Id == 0)
        return ErrorCode::ORDER_ID_INVALID;
    assert((order.Quantity > 0) && "Order quantity must be greater than zero!");
    if (order.Quantity == 0)
        return ErrorCode::ORDER_QUANTITY_INVALID;

    Order new_order(order);

    // Call the corresponding handler
    _market_handler.onAddOrder(new_order);

    // Automatic order matching
    if (_matching)
        MatchMarket((OrderBook*)GetOrderBook(order.SymbolId), &new_order);

    // Call the corresponding handler
    _market_handler.onDeleteOrder(new_order);

    return ErrorCode::OK;
}

ErrorCode MarketManager::AddLimitOrder(const Order& order)
{
    // Validate parameters
    assert((order.Id > 0) && "Order Id must be greater than zero!");
    if (order.Id == 0)
        return ErrorCode::ORDER_ID_INVALID;
    assert((order.Quantity > 0) && "Order quantity must be greater than zero!");
    if (order.Quantity == 0)
        return ErrorCode::ORDER_QUANTITY_INVALID;

    Order new_order(order);

    // Call the corresponding handler
    _market_handler.onAddOrder(new_order);

    // Automatic order matching
    if (_matching)
        MatchLimit((OrderBook*)GetOrderBook(order.SymbolId), &new_order);

    // Add a new order
    if (new_order.Quantity > 0)
    {
        // Create a new order
        OrderNode* order_ptr = _order_pool.Create(new_order);

        // Insert the order
        if (!_orders.insert(std::make_pair(order_ptr->Id, order_ptr)).second)
        {
            // Call the corresponding handler
            _market_handler.onDeleteOrder(*order_ptr);

            // Release the order
            _order_pool.Release(order_ptr);

            return ErrorCode::ORDER_DUPLICATE;
        }

        // Get the valid order book for the new order
        OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
        if (order_book_ptr != nullptr)
        {
            // Add the new limit order into the order book
            UpdateLevel(*order_book_ptr, order_book_ptr->AddLimitOrder(order_ptr));
        }
    }
    else
    {
        // Call the corresponding handler
        _market_handler.onDeleteOrder(new_order);
    }

    return ErrorCode::OK;
}

ErrorCode MarketManager::ReduceOrder(uint64_t id, uint64_t quantity)
{
    // Validate parameters
    assert((id > 0) && "Order Id must be greater than zero!");
    if (id == 0)
        return ErrorCode::ORDER_ID_INVALID;
    assert((quantity > 0) && "Order quantity must be greater than zero!");
    if (quantity == 0)
        return ErrorCode::ORDER_QUANTITY_INVALID;

    // Get the order to reduce
    auto order_it = _orders.find(id);
    assert((order_it != _orders.end()) && "Order not found!");
    if (order_it == _orders.end())
        return ErrorCode::ORDER_NOT_FOUND;
    OrderNode* order_ptr = (OrderNode*)order_it->second;
    assert(order_ptr->IsLimit() && "Reduce order operation is valid only for limit orders!");
    if (!order_ptr->IsLimit())
        return ErrorCode::ORDER_TYPE_INVALID;

    // Calculate the minimal possible order quantity to reduce
    quantity = std::min(quantity, order_ptr->Quantity);

    // Reduce the order quantity
    order_ptr->Quantity -= quantity;

    // Update the order or delete the empty order
    if (order_ptr->Quantity > 0)
    {
        // Call the corresponding handler
        _market_handler.onUpdateOrder(*order_ptr);

        // Get the valid order book for the reducing order
        OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
        if (order_book_ptr != nullptr)
        {
            // Reduce the order in the order book
            UpdateLevel(*order_book_ptr, order_book_ptr->ReduceLimitOrder(order_ptr, quantity));
        }
    }
    else
    {
        // Call the corresponding handler
        _market_handler.onDeleteOrder(*order_ptr);

        // Get the valid order book for the deleted order
        OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
        if (order_book_ptr != nullptr)
        {
            // Reduce the order in the order book
            UpdateLevel(*order_book_ptr, order_book_ptr->ReduceLimitOrder(order_ptr, quantity));
        }

        // Erase the order
        _orders.erase(order_it);

        // Relase the order
        _order_pool.Release(order_ptr);
    }

    return ErrorCode::OK;
}

ErrorCode MarketManager::ModifyOrder(uint64_t id, uint64_t new_price, uint64_t new_quantity)
{
    // Validate parameters
    assert((id > 0) && "Order Id must be greater than zero!");
    if (id == 0)
        return ErrorCode::ORDER_ID_INVALID;

    // Get the order to modify
    auto order_it = _orders.find(id);
    assert((order_it != _orders.end()) && "Order not found!");
    if (order_it == _orders.end())
        return ErrorCode::ORDER_NOT_FOUND;
    OrderNode* order_ptr = (OrderNode*)order_it->second;
    assert(order_ptr->IsLimit() && "Modify order operation is valid only for limit orders!");
    if (!order_ptr->IsLimit())
        return ErrorCode::ORDER_TYPE_INVALID;

    // Get the valid order book for the modifying order
    OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
    if (order_book_ptr != nullptr)
    {
        // Delete the order from the order book
        UpdateLevel(*order_book_ptr, order_book_ptr->DeleteLimitOrder(order_ptr));
    }

    // Modify the order
    order_ptr->Price = new_price;
    order_ptr->Quantity = new_quantity;

    // Update the order
    if (order_ptr->Quantity > 0)
    {
        // Call the corresponding handler
        _market_handler.onUpdateOrder(*order_ptr);

        // Automatic order matching
        if (_matching)
            MatchLimit(order_book_ptr, order_ptr);

        // Add non empty order into the order book
        if (order_ptr->Quantity > 0)
        {
            order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
            if (order_book_ptr != nullptr)
            {
                // Add the modified order into the order book
                UpdateLevel(*order_book_ptr, order_book_ptr->AddLimitOrder(order_ptr));
            }
        }
    }

    // Delete the empty order
    if (order_ptr->Quantity == 0)
    {
        // Call the corresponding handler
        _market_handler.onDeleteOrder(*order_ptr);

        // Erase the order
        _orders.erase(order_it);

        // Relase the order
        _order_pool.Release(order_ptr);
    }

    return ErrorCode::OK;
}

ErrorCode MarketManager::ReplaceOrder(uint64_t id, uint64_t new_id, uint64_t new_price, uint64_t new_quantity)
{
    // Validate parameters
    assert((id > 0) && "Order Id must be greater than zero!");
    if (id == 0)
        return ErrorCode::ORDER_ID_INVALID;
    assert((new_id > 0) && "New order Id must be greater than zero!");
    if (new_id == 0)
        return ErrorCode::ORDER_ID_INVALID;

    // Get the order to replace
    auto order_it = _orders.find(id);
    assert((order_it != _orders.end()) && "Order not found!");
    if (order_it == _orders.end())
        return ErrorCode::ORDER_NOT_FOUND;
    OrderNode* order_ptr = (OrderNode*)order_it->second;
    assert(order_ptr->IsLimit() && "Replace order operation is valid only for limit orders!");
    if (!order_ptr->IsLimit())
        return ErrorCode::ORDER_TYPE_INVALID;

    // Get the valid order book for the replacing order
    OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
    if (order_book_ptr != nullptr)
    {
        // Delete the old order from the order book
        UpdateLevel(*order_book_ptr, order_book_ptr->DeleteLimitOrder(order_ptr));
    }

    // Call the corresponding handler
    _market_handler.onDeleteOrder(*order_ptr);

    // Erase the order
    _orders.erase(order_it);

    // Replace the order
    order_ptr->Id = new_id;
    order_ptr->Price = new_price;
    order_ptr->Quantity = new_quantity;

    // Call the corresponding handler
    _market_handler.onAddOrder(*order_ptr);

    // Automatic order matching
    if (_matching)
        MatchLimit(order_book_ptr, order_ptr);

    if (order_ptr->Quantity > 0)
    {
        // Insert the order
        if (!_orders.insert(std::make_pair(order_ptr->Id, order_ptr)).second)
        {
            // Call the corresponding handler
            _market_handler.onDeleteOrder(*order_ptr);

            // Release the order
            _order_pool.Release(order_ptr);

            return ErrorCode::ORDER_DUPLICATE;
        }

        // Get the valid order book for the new order
        order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
        if (order_book_ptr != nullptr)
        {
            // Add the modified order into the order book
            UpdateLevel(*order_book_ptr, order_book_ptr->AddLimitOrder(order_ptr));
        }
    }
    else
    {
        // Call the corresponding handler
        _market_handler.onDeleteOrder(*order_ptr);

        // Relase the order
        _order_pool.Release(order_ptr);
    }

    return ErrorCode::OK;
}

ErrorCode MarketManager::ReplaceOrder(uint64_t id, const Order& new_order)
{
    // Delete the previous order by Id
    ErrorCode result = DeleteOrder(id);
    if (result != ErrorCode::OK)
        return result;

    // Add the new order
    return AddOrder(new_order);
}

ErrorCode MarketManager::DeleteOrder(uint64_t id)
{
    // Validate parameters
    assert((id > 0) && "Order Id must be greater than zero!");
    if (id == 0)
        return ErrorCode::ORDER_ID_INVALID;

    // Get the order to delete
    auto order_it = _orders.find(id);
    assert((order_it != _orders.end()) && "Order not found!");
    if (order_it == _orders.end())
        return ErrorCode::ORDER_NOT_FOUND;
    OrderNode* order_ptr = (OrderNode*)order_it->second;

    // Get the valid order book for the deleting order
    OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
    if (order_book_ptr != nullptr)
    {
        switch (order_ptr->Type)
        {
            case OrderType::LIMIT:
            {
                // Delete the limit order from the order book
                UpdateLevel(*order_book_ptr, order_book_ptr->DeleteLimitOrder(order_ptr));
                break;
            }
        }
    }

    // Call the corresponding handler
    _market_handler.onDeleteOrder(*order_ptr);

    // Erase the order
    _orders.erase(order_it);

    // Relase the order
    _order_pool.Release(order_ptr);

    return ErrorCode::OK;
}

ErrorCode MarketManager::ExecuteOrder(uint64_t id, uint64_t quantity)
{
    // Validate parameters
    assert((id > 0) && "Order Id must be greater than zero!");
    if (id == 0)
        return ErrorCode::ORDER_ID_INVALID;
    assert((quantity > 0) && "Order quantity must be greater than zero!");
    if (quantity == 0)
        return ErrorCode::ORDER_QUANTITY_INVALID;

    // Get the order to execute
    auto order_it = _orders.find(id);
    assert((order_it != _orders.end()) && "Order not found!");
    if (order_it == _orders.end())
        return ErrorCode::ORDER_NOT_FOUND;
    OrderNode* order_ptr = (OrderNode*)order_it->second;
    assert(order_ptr->IsLimit() && "Execute order operation is valid only for limit orders!");
    if (!order_ptr->IsLimit())
        return ErrorCode::ORDER_TYPE_INVALID;

    // Calculate the minimal possible order quantity to execute
    quantity = std::min(quantity, order_ptr->Quantity);

    // Call the corresponding handler
    _market_handler.onExecuteOrder(*order_ptr, order_ptr->Price, quantity);

    // Reduce the order quantity
    order_ptr->Quantity -= quantity;

    // Get the valid order book for the executing order
    OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
    if (order_book_ptr != nullptr)
    {
        // Reduce the order in the order book
        UpdateLevel(*order_book_ptr, order_book_ptr->ReduceLimitOrder(order_ptr, quantity));
    }

    // Update the order or delete the empty order
    if (order_ptr->Quantity > 0)
    {
        // Call the corresponding handler
        _market_handler.onUpdateOrder(*order_ptr);
    }
    else
    {
        // Call the corresponding handler
        _market_handler.onDeleteOrder(*order_ptr);

        // Erase the order
        _orders.erase(order_it);

        // Relase the order
        _order_pool.Release(order_ptr);
    }

    return ErrorCode::OK;
}

ErrorCode MarketManager::ExecuteOrder(uint64_t id, uint64_t price, uint64_t quantity)
{
    // Validate parameters
    assert((id > 0) && "Order Id must be greater than zero!");
    if (id == 0)
        return ErrorCode::ORDER_ID_INVALID;
    assert((quantity > 0) && "Order quantity must be greater than zero!");
    if (quantity == 0)
        return ErrorCode::ORDER_QUANTITY_INVALID;

    // Get the order to execute
    auto order_it = _orders.find(id);
    assert((order_it != _orders.end()) && "Order not found!");
    if (order_it == _orders.end())
        return ErrorCode::ORDER_NOT_FOUND;
    OrderNode* order_ptr = (OrderNode*)order_it->second;
    assert(order_ptr->IsLimit() && "Execute order operation is valid only for limit orders!");
    if (!order_ptr->IsLimit())
        return ErrorCode::ORDER_TYPE_INVALID;

    // Calculate the minimal possible order quantity to execute
    quantity = std::min(quantity, order_ptr->Quantity);

    // Call the corresponding handler
    _market_handler.onExecuteOrder(*order_ptr, price, quantity);

    // Reduce the order quantity
    order_ptr->Quantity -= quantity;

    // Get the valid order book for the executing order
    OrderBook* order_book_ptr = (OrderBook*)GetOrderBook(order_ptr->SymbolId);
    if (order_book_ptr != nullptr)
    {
        // Reduce the order in the order book
        UpdateLevel(*order_book_ptr, order_book_ptr->ReduceLimitOrder(order_ptr, quantity));
    }

    // Update the order or delete the empty order
    if (order_ptr->Quantity > 0)
    {
        // Call the corresponding handler
        _market_handler.onUpdateOrder(*order_ptr);
    }
    else
    {
        // Call the corresponding handler
        _market_handler.onDeleteOrder(*order_ptr);

        // Erase the order
        _orders.erase(order_it);

        // Relase the order
        _order_pool.Release(order_ptr);
    }

    return ErrorCode::OK;
}

void MarketManager::Match()
{
    for (auto order_book_ptr : _order_books)
        if (order_book_ptr != nullptr)
            Match(order_book_ptr);
}

void MarketManager::Match(OrderBook* order_book_ptr)
{
    // Check the arbitrage bid/ask prices
    while ((order_book_ptr->_best_bid != nullptr) &&
           (order_book_ptr->_best_ask != nullptr) &&
           (order_book_ptr->_best_bid->Price >= order_book_ptr->_best_ask->Price))
    {
        LevelNode* bid = order_book_ptr->_best_bid;
        LevelNode* ask = order_book_ptr->_best_ask;

        // Execute crossed orders
        while (!bid->OrderList.empty() && !ask->OrderList.empty())
        {
            size_t bid_volume = bid->Volume;
            size_t ask_volume = ask->Volume;

            // Find the order to execute and the order to reduce
            Order* executing_order_ptr = bid->OrderList.front();
            Order* reducing_order_ptr = ask->OrderList.front();
            if (executing_order_ptr->Quantity > reducing_order_ptr->Quantity)
                std::swap(executing_order_ptr, reducing_order_ptr);

            // Get the execution quantity
            uint64_t quantity = executing_order_ptr->Quantity;

            // Get the execution price
            uint64_t price = executing_order_ptr->Price;

            // Call the corresponding handler
            _market_handler.onExecuteOrder(*executing_order_ptr, price, quantity);

            // Delete the executing order from the order book
            DeleteOrder(executing_order_ptr->Id);

            // Call the corresponding handler
            _market_handler.onExecuteOrder(*reducing_order_ptr, price, quantity);

            // Reduce the remaining order in the order book
            ReduceOrder(reducing_order_ptr->Id, quantity);

            // If some level becomes empty start again from the best bid/ask level
            if ((quantity == bid_volume) || (quantity == ask_volume))
                break;
        }
    }
}

void MarketManager::MatchMarket(OrderBook* order_book_ptr, Order* order_ptr)
{
    // Check if the order book is valid
    if ((order_book_ptr == nullptr) || (order_ptr == nullptr))
        return;

    // Calculate acceptable marker order price with optional slippage value
    if (order_ptr->IsBuy())
    {
        // Check if there is nothing to buy
        if (order_book_ptr->best_ask() == nullptr)
            return;

        order_ptr->Price = order_book_ptr->best_ask()->Price;
        if (order_ptr->Price > (std::numeric_limits<uint64_t>::max() - order_ptr->Slippage))
            order_ptr->Price = std::numeric_limits<uint64_t>::max();
        else
            order_ptr->Price += order_ptr->Slippage;
    }
    else
    {
        // Check if there is nothing to sell
        if (order_book_ptr->best_bid() == nullptr)
            return;

        order_ptr->Price = order_book_ptr->best_bid()->Price;
        if (order_ptr->Price < (std::numeric_limits<uint64_t>::min() + order_ptr->Slippage))
            order_ptr->Price = std::numeric_limits<uint64_t>::min();
        else
            order_ptr->Price -= order_ptr->Slippage;
    }

    // Match the market order
    MatchOrder(order_book_ptr, order_ptr);
}

void MarketManager::MatchLimit(OrderBook* order_book_ptr, Order* order_ptr)
{
    // Check if the order book is valid
    if ((order_book_ptr == nullptr) || (order_ptr == nullptr))
        return;

    // Match the limit order
    MatchOrder(order_book_ptr, order_ptr);
}

void MarketManager::MatchOrder(OrderBook* order_book_ptr, Order* order_ptr)
{
    // Start the matching from the top of the book
    LevelNode* level;
    while ((level = (order_ptr->IsBuy()) ? order_book_ptr->_best_ask : order_book_ptr->_best_bid) != nullptr)
    {
        // Check the arbitrage bid/ask prices
        bool arbitrage = (order_ptr->IsBuy()) ? (order_ptr->Price >= level->Price) : (order_ptr->Price <= level->Price);
        if (!arbitrage)
            return;

        // Execute crossed orders
        while (!level->OrderList.empty())
        {
            size_t level_volume = level->Volume;

            // Find the order to execute
            Order* executing_order_ptr = level->OrderList.front();

            // Get the execution quantity
            uint64_t quantity = std::min(executing_order_ptr->Quantity, order_ptr->Quantity);

            // Get the execution price
            uint64_t price = executing_order_ptr->Price;

            // Call the corresponding handler
            _market_handler.onExecuteOrder(*executing_order_ptr, price, quantity);

            // Reduce the executing order in the order book
            ReduceOrder(executing_order_ptr->Id, quantity);

            // Call the corresponding handler
            _market_handler.onExecuteOrder(*order_ptr, price, quantity);

            // Reduce quantity to execute
            order_ptr->Quantity -= quantity;
            if (order_ptr->Quantity == 0)
                return;

            // If the level becomes empty start again from the best bid/ask level
            if (quantity == level_volume)
                break;
        }
    }
}

void MarketManager::UpdateLevel(const OrderBook& order_book, const LevelUpdate& update) const
{
    switch (update.Type)
    {
        case UpdateType::ADD:
            _market_handler.onAddLevel(order_book, update.Update, update.Top);
            break;
        case UpdateType::UPDATE:
            _market_handler.onUpdateLevel(order_book, update.Update, update.Top);
            break;
        case UpdateType::DELETE:
            _market_handler.onDeleteLevel(order_book, update.Update, update.Top);
            break;
        default:
            break;
    }

    _market_handler.onUpdateOrderBook(order_book, update.Top);
}

} // namespace Matching
} // namespace CppTrader