#include <Interpreters/ActionsDAG.h>

#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeString.h>
#include <Functions/IFunction.h>
#include <Functions/IFunctionAdaptors.h>
#include <Functions/FunctionsConversion.h>
#include <Functions/materialize.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionJIT.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>

#include <stack>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int DUPLICATE_COLUMN;
    extern const int UNKNOWN_IDENTIFIER;
    extern const int TYPE_MISMATCH;
    extern const int NUMBER_OF_COLUMNS_DOESNT_MATCH;
    extern const int THERE_IS_NO_COLUMN;
    extern const int ILLEGAL_COLUMN;
}


ActionsDAG::ActionsDAG(const NamesAndTypesList & inputs_)
{
    for (const auto & input : inputs_)
        addInput(input.name, input.type, true);
}

ActionsDAG::ActionsDAG(const ColumnsWithTypeAndName & inputs_)
{
    for (const auto & input : inputs_)
    {
        if (input.column && isColumnConst(*input.column))
            addInput(input, true);
        else
            addInput(input.name, input.type, true);
    }
}

ActionsDAG::Node & ActionsDAG::addNode(Node node, bool can_replace)
{
    auto it = index.find(node.result_name);
    if (it != index.end() && !can_replace)
        throw Exception("Column '" + node.result_name + "' already exists", ErrorCodes::DUPLICATE_COLUMN);

    auto & res = nodes.emplace_back(std::move(node));

    if (res.type == ActionType::INPUT)
        inputs.emplace_back(&res);

    index.replace(&res);
    return res;
}

ActionsDAG::Node & ActionsDAG::getNode(const std::string & name)
{
    auto it = index.find(name);
    if (it == index.end())
        throw Exception("Unknown identifier: '" + name + "'", ErrorCodes::UNKNOWN_IDENTIFIER);

    return **it;
}

const ActionsDAG::Node & ActionsDAG::addInput(std::string name, DataTypePtr type, bool can_replace)
{
    Node node;
    node.type = ActionType::INPUT;
    node.result_type = std::move(type);
    node.result_name = std::move(name);

    return addNode(std::move(node), can_replace);
}

const ActionsDAG::Node & ActionsDAG::addInput(ColumnWithTypeAndName column, bool can_replace)
{
    Node node;
    node.type = ActionType::INPUT;
    node.result_type = std::move(column.type);
    node.result_name = std::move(column.name);
    node.column = std::move(column.column);

    return addNode(std::move(node), can_replace);
}

const ActionsDAG::Node & ActionsDAG::addColumn(ColumnWithTypeAndName column, bool can_replace)
{
    if (!column.column)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot add column {} because it is nullptr", column.name);

    Node node;
    node.type = ActionType::COLUMN;
    node.result_type = std::move(column.type);
    node.result_name = std::move(column.name);
    node.column = std::move(column.column);

    return addNode(std::move(node), can_replace);
}

const ActionsDAG::Node & ActionsDAG::addAlias(const std::string & name, std::string alias, bool can_replace)
{
    return addAlias(getNode(name), alias, can_replace);
}

ActionsDAG::Node & ActionsDAG::addAlias(Node & child, std::string alias, bool can_replace)
{
    Node node;
    node.type = ActionType::ALIAS;
    node.result_type = child.result_type;
    node.result_name = std::move(alias);
    node.column = child.column;
    node.allow_constant_folding = child.allow_constant_folding;
    node.children.emplace_back(&child);

    return addNode(std::move(node), can_replace);
}

const ActionsDAG::Node & ActionsDAG::addArrayJoin(const std::string & source_name, std::string result_name)
{
    auto & child = getNode(source_name);

    const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(child.result_type.get());
    if (!array_type)
        throw Exception("ARRAY JOIN requires array argument", ErrorCodes::TYPE_MISMATCH);

    Node node;
    node.type = ActionType::ARRAY_JOIN;
    node.result_type = array_type->getNestedType();
    node.result_name = std::move(result_name);
    node.children.emplace_back(&child);

    return addNode(std::move(node));
}

const ActionsDAG::Node & ActionsDAG::addFunction(
        const FunctionOverloadResolverPtr & function,
        const Names & argument_names,
        std::string result_name,
        const Context & context [[maybe_unused]])
{
    const auto & all_settings = context.getSettingsRef();
    settings.max_temporary_columns = all_settings.max_temporary_columns;
    settings.max_temporary_non_const_columns = all_settings.max_temporary_non_const_columns;

#if USE_EMBEDDED_COMPILER
    settings.compile_expressions = all_settings.compile_expressions;
    settings.min_count_to_compile_expression = all_settings.min_count_to_compile_expression;

    if (!compilation_cache)
        compilation_cache = context.getCompiledExpressionCache();
#endif

    Inputs children;
    children.reserve(argument_names.size());
    for (const auto & name : argument_names)
        children.push_back(&getNode(name));

    return addFunction(function, children, std::move(result_name), false);
}

ActionsDAG::Node & ActionsDAG::addFunction(
        const FunctionOverloadResolverPtr & function,
        Inputs children,
        std::string result_name,
        bool can_replace)
{
    size_t num_arguments = children.size();

    Node node;
    node.type = ActionType::FUNCTION;
    node.function_builder = function;
    node.children = std::move(children);

    bool all_const = true;
    ColumnsWithTypeAndName arguments(num_arguments);

    for (size_t i = 0; i < num_arguments; ++i)
    {
        auto & child = *node.children[i];
        node.allow_constant_folding = node.allow_constant_folding && child.allow_constant_folding;

        ColumnWithTypeAndName argument;
        argument.column = child.column;
        argument.type = child.result_type;
        argument.name = child.result_name;

        if (!argument.column || !isColumnConst(*argument.column))
            all_const = false;

        arguments[i] = std::move(argument);
    }

    node.function_base = function->build(arguments);
    node.result_type = node.function_base->getResultType();
    node.function = node.function_base->prepare(arguments);

    /// If all arguments are constants, and function is suitable to be executed in 'prepare' stage - execute function.
    /// But if we compile expressions compiled version of this function maybe placed in cache,
    /// so we don't want to unfold non deterministic functions
    if (all_const && node.function_base->isSuitableForConstantFolding()
        && (!settings.compile_expressions || node.function_base->isDeterministic()))
    {
        size_t num_rows = arguments.empty() ? 0 : arguments.front().column->size();
        auto col = node.function->execute(arguments, node.result_type, num_rows, true);

        /// If the result is not a constant, just in case, we will consider the result as unknown.
        if (isColumnConst(*col))
        {
            /// All constant (literal) columns in block are added with size 1.
            /// But if there was no columns in block before executing a function, the result has size 0.
            /// Change the size to 1.

            if (col->empty())
                col = col->cloneResized(1);

            node.column = std::move(col);
        }
    }

    /// Some functions like ignore() or getTypeName() always return constant result even if arguments are not constant.
    /// We can't do constant folding, but can specify in sample block that function result is constant to avoid
    /// unnecessary materialization.
    if (!node.column && node.function_base->isSuitableForConstantFolding())
    {
        if (auto col = node.function_base->getResultIfAlwaysReturnsConstantAndHasArguments(arguments))
        {
            node.column = std::move(col);
            node.allow_constant_folding = false;
        }
    }

    if (result_name.empty())
    {
        result_name = function->getName() + "(";
        for (size_t i = 0; i < num_arguments; ++i)
        {
            if (i)
                result_name += ", ";
            result_name += node.children[i]->result_name;
        }
        result_name += ")";
    }

    node.result_name = std::move(result_name);

    return addNode(std::move(node), can_replace);
}


NamesAndTypesList ActionsDAG::getRequiredColumns() const
{
    NamesAndTypesList result;
    for (const auto & input : inputs)
        result.emplace_back(input->result_name, input->result_type);

    return result;
}

ColumnsWithTypeAndName ActionsDAG::getResultColumns() const
{
    ColumnsWithTypeAndName result;
    result.reserve(index.size());
    for (const auto & node : index)
        result.emplace_back(node->column, node->result_type, node->result_name);

    return result;
}

NamesAndTypesList ActionsDAG::getNamesAndTypesList() const
{
    NamesAndTypesList result;
    for (const auto & node : index)
        result.emplace_back(node->result_name, node->result_type);

    return result;
}

Names ActionsDAG::getNames() const
{
    Names names;
    names.reserve(index.size());
    for (const auto & node : index)
        names.emplace_back(node->result_name);

    return names;
}

std::string ActionsDAG::dumpNames() const
{
    WriteBufferFromOwnString out;
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        if (it != nodes.begin())
            out << ", ";
        out << it->result_name;
    }
    return out.str();
}

void ActionsDAG::removeUnusedActions(const Names & required_names)
{
    std::unordered_set<Node *> nodes_set;
    std::vector<Node *> required_nodes;
    required_nodes.reserve(required_names.size());

    for (const auto & name : required_names)
    {
        auto it = index.find(name);
        if (it == index.end())
            throw Exception(ErrorCodes::UNKNOWN_IDENTIFIER,
                            "Unknown column: {}, there are only columns {}", name, dumpNames());

        if (nodes_set.insert(*it).second)
            required_nodes.push_back(*it);
    }

    removeUnusedActions(required_nodes);
}

void ActionsDAG::removeUnusedActions(const std::vector<Node *> & required_nodes)
{
    {
        Index new_index;

        for (auto * node : required_nodes)
            new_index.insert(node);

        index.swap(new_index);
    }

    removeUnusedActions();
}

void ActionsDAG::removeUnusedActions()
{
    std::unordered_set<const Node *> visited_nodes;
    std::stack<Node *> stack;

    for (auto * node : index)
    {
        visited_nodes.insert(node);
        stack.push(node);
    }

    /// We cannot remove arrayJoin because it changes the number of rows.
    for (auto & node : nodes)
    {
        if (node.type == ActionType::ARRAY_JOIN && visited_nodes.count(&node) == 0)
        {
            visited_nodes.insert(&node);
            stack.push(&node);
        }
    }

    while (!stack.empty())
    {
        auto * node = stack.top();
        stack.pop();

        if (!node->children.empty() && node->column && isColumnConst(*node->column) && node->allow_constant_folding)
        {
            /// Constant folding.
            node->type = ActionsDAG::ActionType::COLUMN;
            node->children.clear();
        }

        for (auto * child : node->children)
        {
            if (visited_nodes.count(child) == 0)
            {
                stack.push(child);
                visited_nodes.insert(child);
            }
        }
    }

    nodes.remove_if([&](const Node & node) { return visited_nodes.count(&node) == 0; });
    auto it = std::remove_if(inputs.begin(), inputs.end(), [&](const Node * node) { return visited_nodes.count(node) == 0; });
    inputs.erase(it, inputs.end());
}

void ActionsDAG::addAliases(const NamesWithAliases & aliases, std::vector<Node *> & result_nodes)
{
    std::vector<Node *> required_nodes;

    for (const auto & item : aliases)
    {
        auto & child = getNode(item.first);
        required_nodes.push_back(&child);
    }

    result_nodes.reserve(aliases.size());

    for (size_t i = 0; i < aliases.size(); ++i)
    {
        const auto & item = aliases[i];
        auto * child = required_nodes[i];

        if (!item.second.empty() && item.first != item.second)
        {
            Node node;
            node.type = ActionType::ALIAS;
            node.result_type = child->result_type;
            node.result_name = std::move(item.second);
            node.column = child->column;
            node.allow_constant_folding = child->allow_constant_folding;
            node.children.emplace_back(child);

            auto & alias = addNode(std::move(node), true);
            result_nodes.push_back(&alias);
        }
        else
            result_nodes.push_back(child);
    }
}

void ActionsDAG::addAliases(const NamesWithAliases & aliases)
{
    std::vector<Node *> result_nodes;
    addAliases(aliases, result_nodes);
}

void ActionsDAG::project(const NamesWithAliases & projection)
{
    std::vector<Node *> result_nodes;
    addAliases(projection, result_nodes);
    removeUnusedActions(result_nodes);
    projectInput();
    settings.projected_output = true;
}

bool ActionsDAG::tryRestoreColumn(const std::string & column_name)
{
    if (index.contains(column_name))
        return true;

    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
    {
        auto & node = *it;
        if (node.result_name == column_name)
        {
            index.replace(&node);
            return true;
        }
    }

    return false;
}

void ActionsDAG::removeUnusedInput(const std::string & column_name)
{
    auto it = inputs.begin();
    for (; it != inputs.end(); ++it)
        if ((*it)->result_name == column_name)
            break;

    if (it == inputs.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Not found input {} in ActionsDAG\n{}", column_name, dumpDAG());

    auto * input = *it;
    for (const auto & node : nodes)
        for (const auto * child : node.children)
            if (input == child)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                                "Cannot remove input {} because it has dependent nodes in ActionsDAG\n{}",
                                column_name, dumpDAG());

    for (auto jt = index.begin(); jt != index.end(); ++jt)
    {
        if (*jt == input)
        {
            index.remove(jt);
            break;
        }
    }

    for (auto jt = nodes.begin(); jt != nodes.end(); ++jt)
    {
        if (&(*jt) == input)
        {
            nodes.erase(jt);
            break;
        }
    }

    inputs.erase(it);
}

ActionsDAGPtr ActionsDAG::clone() const
{
    auto actions = cloneEmpty();

    std::unordered_map<const Node *, Node *> copy_map;

    for (const auto & node : nodes)
    {
        auto & copy_node = actions->nodes.emplace_back(node);
        copy_map[&node] = &copy_node;
    }

    for (auto & node : actions->nodes)
        for (auto & child : node.children)
            child = copy_map[child];

    for (const auto & node : index)
        actions->index.insert(copy_map[node]);

    for (const auto & node : inputs)
        actions->inputs.push_back(copy_map[node]);

    return actions;
}

void ActionsDAG::compileExpressions()
{
#if USE_EMBEDDED_COMPILER
    if (settings.compile_expressions)
    {
        compileFunctions();
        removeUnusedActions();
    }
#endif
}

std::string ActionsDAG::dumpDAG() const
{
    std::unordered_map<const Node *, size_t> map;
    for (const auto & node : nodes)
    {
        size_t idx = map.size();
        map[&node] = idx;
    }

    WriteBufferFromOwnString out;
    for (const auto & node : nodes)
    {
        out << map[&node] << " : ";
        switch (node.type)
        {
            case ActionsDAG::ActionType::COLUMN:
                out << "COLUMN ";
                break;

            case ActionsDAG::ActionType::ALIAS:
                out << "ALIAS ";
                break;

            case ActionsDAG::ActionType::FUNCTION:
                out << "FUNCTION ";
                break;

            case ActionsDAG::ActionType::ARRAY_JOIN:
                out << "ARRAY JOIN ";
                break;

            case ActionsDAG::ActionType::INPUT:
                out << "INPUT ";
                break;
        }

        out << "(";
        for (size_t i = 0; i < node.children.size(); ++i)
        {
            if (i)
                out << ", ";
            out << map[node.children[i]];
        }
        out << ")";

        out << " " << (node.column ? node.column->getName() : "(no column)");
        out << " " << (node.result_type ? node.result_type->getName() : "(no type)");
        out << " " << (!node.result_name.empty() ? node.result_name : "(no name)");
        if (node.function_base)
            out << " [" << node.function_base->getName() << "]";

        out << "\n";
    }

    out << "Index:";
    for (const auto * node : index)
        out << ' ' << map[node];
    out << '\n';

    return out.str();
}

bool ActionsDAG::hasArrayJoin() const
{
    for (const auto & node : nodes)
        if (node.type == ActionType::ARRAY_JOIN)
            return true;

    return false;
}

bool ActionsDAG::hasStatefulFunctions() const
{
    for (const auto & node : nodes)
        if (node.type == ActionType::FUNCTION && node.function_base->isStateful())
            return true;

    return false;
}

bool ActionsDAG::empty() const
{
    for (const auto & node : nodes)
        if (node.type != ActionType::INPUT)
            return false;

    return true;
}

ActionsDAGPtr ActionsDAG::makeConvertingActions(
    const ColumnsWithTypeAndName & source,
    const ColumnsWithTypeAndName & result,
    MatchColumnsMode mode,
    bool ignore_constant_values)
{
    size_t num_input_columns = source.size();
    size_t num_result_columns = result.size();

    if (mode == MatchColumnsMode::Position && num_input_columns != num_result_columns)
        throw Exception("Number of columns doesn't match", ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH);

    auto actions_dag = std::make_shared<ActionsDAG>(source);
    std::vector<Node *> projection(num_result_columns);

    FunctionOverloadResolverPtr func_builder_materialize =
            std::make_shared<FunctionOverloadResolverAdaptor>(
                    std::make_unique<DefaultOverloadResolver>(
                            std::make_shared<FunctionMaterialize>()));

    std::map<std::string_view, std::list<size_t>> inputs;
    if (mode == MatchColumnsMode::Name)
    {
        for (size_t pos = 0; pos < actions_dag->inputs.size(); ++pos)
            inputs[actions_dag->inputs[pos]->result_name].push_back(pos);
    }

    for (size_t result_col_num = 0; result_col_num < num_result_columns; ++result_col_num)
    {
        const auto & res_elem = result[result_col_num];
        Node * src_node = nullptr;

        switch (mode)
        {
            case MatchColumnsMode::Position:
            {
                src_node = actions_dag->inputs[result_col_num];
                break;
            }

            case MatchColumnsMode::Name:
            {
                auto & input = inputs[res_elem.name];
                if (input.empty())
                    throw Exception("Cannot find column " + backQuote(res_elem.name) + " in source stream",
                                    ErrorCodes::THERE_IS_NO_COLUMN);

                src_node = actions_dag->inputs[input.front()];
                input.pop_front();
                break;
            }
        }

        /// Check constants.
        if (const auto * res_const = typeid_cast<const ColumnConst *>(res_elem.column.get()))
        {
            if (const auto * src_const = typeid_cast<const ColumnConst *>(src_node->column.get()))
            {
                if (ignore_constant_values)
                   src_node = const_cast<Node *>(&actions_dag->addColumn(res_elem, true));
                else if (res_const->getField() != src_const->getField())
                    throw Exception("Cannot convert column " + backQuote(res_elem.name) + " because "
                                    "it is constant but values of constants are different in source and result",
                                    ErrorCodes::ILLEGAL_COLUMN);
            }
            else
                throw Exception("Cannot convert column " + backQuote(res_elem.name) + " because "
                                "it is non constant in source stream but must be constant in result",
                                ErrorCodes::ILLEGAL_COLUMN);
        }

        /// Add CAST function to convert into result type if needed.
        if (!res_elem.type->equals(*src_node->result_type))
        {
            ColumnWithTypeAndName column;
            column.name = res_elem.type->getName();
            column.column = DataTypeString().createColumnConst(0, column.name);
            column.type = std::make_shared<DataTypeString>();

            auto * right_arg = const_cast<Node *>(&actions_dag->addColumn(std::move(column), true));
            auto * left_arg = src_node;

            FunctionCast::Diagnostic diagnostic = {src_node->result_name, res_elem.name};
            FunctionOverloadResolverPtr func_builder_cast =
                    std::make_shared<FunctionOverloadResolverAdaptor>(
                            CastOverloadResolver<CastType::nonAccurate>::createImpl(false, std::move(diagnostic)));

            Inputs children = { left_arg, right_arg };
            src_node = &actions_dag->addFunction(func_builder_cast, std::move(children), {}, true);
        }

        if (src_node->column && isColumnConst(*src_node->column) && !(res_elem.column && isColumnConst(*res_elem.column)))
        {
            Inputs children = {src_node};
            src_node = &actions_dag->addFunction(func_builder_materialize, std::move(children), {}, true);
        }

        if (src_node->result_name != res_elem.name)
            src_node = &actions_dag->addAlias(*src_node, res_elem.name, true);

        projection[result_col_num] = src_node;
    }

    actions_dag->removeUnusedActions(projection);
    actions_dag->projectInput();

    return actions_dag;
}

ActionsDAGPtr ActionsDAG::merge(ActionsDAG && first, ActionsDAG && second)
{
    /// first: x (1), x (2), y ==> x (2), z, x (3)
    /// second: x (1), x (2), x (3) ==> x (3), x (2), x (1)
    /// merge: x (1), x (2), x (3), y =(first)=> x (2), z, x (4), x (3) =(second)=> x (3), x (4), x (2), z

    /// Will store merged result in `first`.

    /// This map contains nodes which should be removed from `first` index, cause they are used as inputs for `second`.
    /// The second element is the number of removes (cause one node may be repeated several times in result).
    std::unordered_map<Node *, size_t> removed_first_result;
    /// Map inputs of `second` to nodes of `first`.
    std::unordered_map<Node *, Node *> inputs_map;

    /// Update inputs list.
    {
        /// Index may have multiple columns with same name. They also may be used by `second`. Order is important.
        std::unordered_map<std::string_view, std::list<Node *>> first_result;
        for (auto & node : first.index)
            first_result[node->result_name].push_back(node);

        for (auto & node : second.inputs)
        {
            auto it = first_result.find(node->result_name);
            if (it == first_result.end() || it->second.empty())
            {
                if (first.settings.project_input)
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                                    "Cannot find column {} in ActionsDAG result", node->result_name);

                first.inputs.push_back(node);
            }
            else
            {
                inputs_map[node] = it->second.front();
                removed_first_result[it->second.front()] += 1;
                it->second.pop_front();
            }
        }
    }

    /// Replace inputs from `second` to nodes from `first` result.
    for (auto & node : second.nodes)
    {
        for (auto & child : node.children)
        {
            if (child->type == ActionType::INPUT)
            {
                auto it = inputs_map.find(child);
                if (it != inputs_map.end())
                    child = it->second;
            }
        }
    }

    for (auto & node : second.index)
    {
        if (node->type == ActionType::INPUT)
        {
            auto it = inputs_map.find(node);
            if (it != inputs_map.end())
                node = it->second;
        }
    }

    /// Update index.
    if (second.settings.project_input)
    {
        first.index.swap(second.index);
        first.settings.project_input = true;
    }
    else
    {
        /// Remove `second` inputs from index.
        for (auto it = first.index.begin(); it != first.index.end();)
        {
            auto cur = it;
            ++it;

            auto jt = removed_first_result.find(*cur);
            if (jt != removed_first_result.end() && jt->second > 0)
            {
                first.index.remove(cur);
                --jt->second;
            }
        }

        for (auto it = second.index.rbegin(); it != second.index.rend(); ++it)
            first.index.prepend(*it);
    }


    first.nodes.splice(first.nodes.end(), std::move(second.nodes));

    /// Here we rebuild index because some string_view from the first map now may point to string from second.
    ActionsDAG::Index first_index;
    for (auto * node : first.index)
        first_index.insert(node);

    first.index.swap(first_index);

#if USE_EMBEDDED_COMPILER
    if (first.compilation_cache == nullptr)
        first.compilation_cache = second.compilation_cache;
#endif

    first.settings.max_temporary_columns = std::max(first.settings.max_temporary_columns, second.settings.max_temporary_columns);
    first.settings.max_temporary_non_const_columns = std::max(first.settings.max_temporary_non_const_columns, second.settings.max_temporary_non_const_columns);
    first.settings.min_count_to_compile_expression = std::max(first.settings.min_count_to_compile_expression, second.settings.min_count_to_compile_expression);
    first.settings.projected_output = second.settings.projected_output;

    /// Drop unused inputs and, probably, some actions.
    first.removeUnusedActions();

    return std::make_shared<ActionsDAG>(std::move(first));
}

std::pair<ActionsDAGPtr, ActionsDAGPtr> ActionsDAG::split(std::unordered_set<const Node *> split_nodes) const
{
    /// Split DAG into two parts.
    /// (first_nodes, first_index) is a part which will have split_list in result.
    /// (second_nodes, second_index) is a part which will have same index as current actions.
    std::list<Node> second_nodes;
    std::list<Node> first_nodes;
    Index second_index;
    Index first_index;

    /// List of nodes from current actions which are not inputs, but will be in second part.
    std::vector<const Node *> new_inputs;

    struct Frame
    {
        const Node * node;
        size_t next_child_to_visit = 0;
    };

    struct Data
    {
        bool needed_by_split_node = false;
        bool visited = false;
        bool used_in_result = false;

        /// Copies of node in one of the DAGs.
        /// For COLUMN and INPUT both copies may exist.
        Node * to_second = nullptr;
        Node * to_first = nullptr;
    };

    std::stack<Frame> stack;
    std::unordered_map<const Node *, Data> data;

    for (const auto & node : index)
        data[node].used_in_result = true;

    /// DFS. Decide if node is needed by split.
    for (const auto & node : nodes)
    {
        if (split_nodes.count(&node) == 0)
            continue;

        auto & cur_data = data[&node];
        if (cur_data.needed_by_split_node)
            continue;

        cur_data.needed_by_split_node = true;
        stack.push({.node = &node});

        while (!stack.empty())
        {
            auto & cur_node = stack.top().node;
            stack.pop();

            for (const auto * child : cur_node->children)
            {
                auto & child_data = data[child];
                if (!child_data.needed_by_split_node)
                {
                    child_data.needed_by_split_node = true;
                    stack.push({.node = child});
                }
            }
        }
    }

    /// DFS. Move nodes to one of the DAGs.
    for (const auto & node : nodes)
    {
        if (!data[&node].visited)
            stack.push({.node = &node});

        while (!stack.empty())
        {
            auto & cur = stack.top();
            auto & cur_data = data[cur.node];

            /// At first, visit all children.
            while (cur.next_child_to_visit < cur.node->children.size())
            {
                auto * child = cur.node->children[cur.next_child_to_visit];
                auto & child_data = data[child];

                if (!child_data.visited)
                {
                    stack.push({.node = child});
                    break;
                }

                ++cur.next_child_to_visit;
            }

            /// Make a copy part.
            if (cur.next_child_to_visit == cur.node->children.size())
            {
                cur_data.visited = true;
                stack.pop();

                if (!cur_data.needed_by_split_node)
                {
                    auto & copy = second_nodes.emplace_back(*cur.node);
                    cur_data.to_second = &copy;

                    /// Replace children to newly created nodes.
                    for (auto & child : copy.children)
                    {
                        auto & child_data = data[child];

                        /// If children is not created, it may be from split part.
                        if (!child_data.to_second)
                        {
                            if (child->type == ActionType::COLUMN) /// Just create new node for COLUMN action.
                            {
                                child_data.to_second = &second_nodes.emplace_back(*child);
                            }
                            else
                            {
                                /// Node from first part is added as new input.
                                Node input_node;
                                input_node.type = ActionType::INPUT;
                                input_node.result_type = child->result_type;
                                input_node.result_name = child->result_name;
                                child_data.to_second = &second_nodes.emplace_back(std::move(input_node));

                                new_inputs.push_back(child);
                            }
                        }

                        child = child_data.to_second;
                    }

                    /// Input from second DAG should also be in the first.
                    if (copy.type == ActionType::INPUT)
                    {
                        auto & input_copy = first_nodes.emplace_back(*cur.node);
                        assert(cur_data.to_first == nullptr);
                        cur_data.to_first = &input_copy;
                        new_inputs.push_back(cur.node);
                    }
                }
                else
                {
                    auto & copy = first_nodes.emplace_back(*cur.node);
                    cur_data.to_first = &copy;

                    /// Replace children to newly created nodes.
                    for (auto & child : copy.children)
                    {
                        child = data[child].to_first;
                        assert(child != nullptr);
                    }

                    if (cur_data.used_in_result)
                    {
                        /// If this node is needed in result, add it as input.
                        Node input_node;
                        input_node.type = ActionType::INPUT;
                        input_node.result_type = node.result_type;
                        input_node.result_name = node.result_name;
                        cur_data.to_second = &second_nodes.emplace_back(std::move(input_node));

                        new_inputs.push_back(cur.node);
                    }
                }
            }
        }
    }

    for (auto * node : index)
        second_index.insert(data[node].to_second);

    Inputs second_inputs;
    Inputs first_inputs;

    for (auto * input : inputs)
    {
        const auto & cur = data[input];
        first_inputs.push_back(cur.to_first);
    }

    for (const auto * input : new_inputs)
    {
        const auto & cur = data[input];
        second_inputs.push_back(cur.to_second);
        first_index.insert(cur.to_first);
    }

    auto first_actions = cloneEmpty();
    first_actions->nodes.swap(first_nodes);
    first_actions->index.swap(first_index);
    first_actions->inputs.swap(first_inputs);

    auto second_actions = cloneEmpty();
    second_actions->nodes.swap(second_nodes);
    second_actions->index.swap(second_index);
    second_actions->inputs.swap(second_inputs);

    return {std::move(first_actions), std::move(second_actions)};
}

std::pair<ActionsDAGPtr, ActionsDAGPtr>  ActionsDAG::splitActionsBeforeArrayJoin(const NameSet & array_joined_columns) const
{

    struct Frame
    {
        const Node * node;
        size_t next_child_to_visit = 0;
    };

    std::unordered_set<const Node *> split_nodes;
    std::unordered_set<const Node *> visited_nodes;

    std::stack<Frame> stack;

    /// DFS. Decide if node depends on ARRAY JOIN.
    for (const auto & node : nodes)
    {
        if (visited_nodes.count(&node))
            continue;

        visited_nodes.insert(&node);
        stack.push({.node = &node});

        while (!stack.empty())
        {
            auto & cur = stack.top();

            /// At first, visit all children. We depend on ARRAY JOIN if any child does.
            while (cur.next_child_to_visit < cur.node->children.size())
            {
                auto * child = cur.node->children[cur.next_child_to_visit];

                if (visited_nodes.count(child) == 0)
                {
                    visited_nodes.insert(child);
                    stack.push({.node = child});
                    break;
                }

                ++cur.next_child_to_visit;
            }

            if (cur.next_child_to_visit == cur.node->children.size())
            {
                bool depend_on_array_join = false;
                if (cur.node->type == ActionType::INPUT && array_joined_columns.count(cur.node->result_name))
                    depend_on_array_join = true;

                for (const auto * child : cur.node->children)
                {
                    if (split_nodes.count(child) == 0)
                        depend_on_array_join = true;
                }

                if (!depend_on_array_join)
                    split_nodes.insert(cur.node);

                stack.pop();
            }
        }
    }

    auto res = split(split_nodes);
    /// Do not remove array joined columns if they are not used.
    res.first->settings.project_input = false;
    return res;
}

std::pair<ActionsDAGPtr, ActionsDAGPtr> ActionsDAG::splitActionsForFilter(const std::string & column_name) const
{
    auto it = index.begin();
    for (; it != index.end(); ++it)
        if ((*it)->result_name == column_name)
            break;

    if (it == index.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "Index for ActionsDAG does not contain filter column name {}. DAG:\n{}",
                        column_name, dumpDAG());

    std::unordered_set<const Node *> split_nodes = {*it};
    return split(split_nodes);
}

}
