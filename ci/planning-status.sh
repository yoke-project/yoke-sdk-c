#!/usr/bin/env bash
# Moves forward the planning item of everything this change names. It verifies nothing and
# blocks nothing: with no credential it says so and exits zero, and every failure of its own is
# reported and never propagated.
#
# It moves an item forward only — Todo, then In Progress, then In Review — and never to Done, which
# the planning tool sets itself when the issue closes.
#
# Usage: planning-status.sh <push|pull_request>
# Reads: GH_TOKEN, GITHUB_REPOSITORY, GITHUB_EVENT_PATH, PLANNING_ORG, PLANNING_PROJECT.
set -uo pipefail

# The states this workflow moves an item through, in order. Done is deliberately absent.
planning_states=("Todo" "In Progress" "In Review")

# forward prints the state to move to, or nothing when the item is already there or past it. An item
# with no state at all — one just added to the plan — is before every state, and so moves to either.
forward() {
  local current="$1" target="$2" i place=-1 destination=-1
  for i in "${!planning_states[@]}"; do
    [[ "${planning_states[$i]}" == "$current" ]] && place=$i
    [[ "${planning_states[$i]}" == "$target" ]] && destination=$i
  done
  (( destination < 0 )) && return 0                        # not a state this workflow moves an item to
  [[ -n "$current" ]] && (( place < 0 )) && return 0       # Done, or a state nobody here moves through
  (( destination > place )) && echo "$target"
  return 0
}

# items_named_by_push prints every item the commits of a push name, each qualified by its repository.
items_named_by_push() {
  local event="$1" repository="$2"
  jq -r '.commits[]?.message' "$event" 2>/dev/null \
    | grep -oE '([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)?#[0-9]+' \
    | sed -E "s|^#|$repository#|" \
    | sort -u
}

# items_closed_by_change prints the items a proposed change closes, as the forge itself resolved them.
# It fails, printing nothing, when the query does — an error document read as data is a wrong item.
items_closed_by_change() {
  local repository="$1" number="$2" answer
  answer="$(gh api graphql -f owner="${repository%%/*}" -f name="${repository##*/}" -F number="$number" -f query='
    query($owner:String!,$name:String!,$number:Int!){
      repository(owner:$owner,name:$name){
        pullRequest(number:$number){
          closingIssuesReferences(first:20){
            nodes{ number repository{ nameWithOwner } }
          }
        }
      }
    }' --jq '.data.repository.pullRequest.closingIssuesReferences.nodes[]
             | "\(.repository.nameWithOwner)#\(.number)"' 2>/dev/null)" || return 1
  sort -u <<<"$answer"
}

# planning_field prints the project's identifier, its Status field's, and the identifier of one option.
planning_field() {
  local organisation="$1" project="$2" option="$3" answer
  answer="$(gh api graphql -f organisation="$organisation" -F project="$project" -f query='
    query($organisation:String!,$project:Int!){
      organization(login:$organisation){
        projectV2(number:$project){
          id
          field(name:"Status"){ ... on ProjectV2SingleSelectField { id options { id name } } }
        }
      }
    }' --jq "[.data.organization.projectV2.id,
              .data.organization.projectV2.field.id,
              (.data.organization.projectV2.field.options[] | select(.name==\"$option\") | .id)]
             | @tsv" 2>/dev/null)" || return 1
  printf '%s\n' "$answer"
}

# item_in_project prints the identifier of an issue's item in the plan, and the state it is in. An
# issue commonly sits in more than one project, so the plan's own number is what picks its item out.
item_in_project() {
  local repository="$1" number="$2" project="$3"
  [[ "$project" =~ ^[0-9]+$ ]] || return 1
  local answer
  answer="$(gh api graphql -f owner="${repository%%/*}" -f name="${repository##*/}" -F number="$number" \
    -f query='
    query($owner:String!,$name:String!,$number:Int!){
      repository(owner:$owner,name:$name){
        issue(number:$number){
          projectItems(first:20){
            nodes{
              id
              project{ number }
              fieldValueByName(name:"Status"){
                ... on ProjectV2ItemFieldSingleSelectValue { name }
              }
            }
          }
        }
      }
    }' --jq "[.data.repository.issue.projectItems.nodes[]
              | select(.project.number==$project)
              | [.id, (.fieldValueByName.name // \"\")]][0] | @tsv" 2>/dev/null)" || return 1
  printf '%s\n' "$answer"
}

# move sets one item's state, and says what it did either way.
move() {
  local item="$1" target="$2" organisation="$3" project="$4"
  local repository="${item%%#*}" number="${item##*#}"

  local found place state
  if ! found="$(item_in_project "$repository" "$number" "$project")"; then
    echo "planning: $item could not be read from the plan; nothing moved"
    return 0
  fi
  if [[ -z "$found" ]]; then
    echo "planning: $item is not in the plan; nothing moved"
    return 0
  fi
  IFS=$'\t' read -r place state <<<"$found"

  local destination
  destination="$(forward "$state" "$target")"
  if [[ -z "$destination" ]]; then
    echo "planning: $item is ${state:-unset}, and $target is not forward of it"
    return 0
  fi

  local ids project_id field_id option_id
  if ! ids="$(planning_field "$organisation" "$project" "$destination")"; then
    echo "planning: the plan's own fields could not be read; nothing moved"
    return 0
  fi
  IFS=$'\t' read -r project_id field_id option_id <<<"$ids"
  if [[ -z "$option_id" ]]; then
    echo "planning: the plan has no state $destination; nothing moved"
    return 0
  fi

  if gh api graphql -f project="$project_id" -f item="$place" -f field="$field_id" -f option="$option_id" \
    -f query='
      mutation($project:ID!,$item:ID!,$field:ID!,$option:String!){
        updateProjectV2ItemFieldValue(input:{
          projectId:$project, itemId:$item, fieldId:$field,
          value:{ singleSelectOptionId:$option }
        }){ projectV2Item { id } }
      }' >/dev/null 2>&1; then
    echo "planning: $item moved from ${state:-unset} to $destination"
  else
    echo "planning: $item could not be moved to $destination"
  fi
}

main() {
  local event_name="${1:-}"

  if [[ -z "${GH_TOKEN:-}" ]]; then
    echo "planning: no credential configured; nothing moved"
    return 0
  fi

  local organisation="${PLANNING_ORG:-}" project="${PLANNING_PROJECT:-}"
  if [[ -z "$organisation" || -z "$project" ]]; then
    echo "planning: no plan named; nothing moved"
    return 0
  fi

  local repository="${GITHUB_REPOSITORY:-}" event="${GITHUB_EVENT_PATH:-}"
  local target items
  case "$event_name" in
    push)
      target="In Progress"
      items="$(items_named_by_push "$event" "$repository")"
      ;;
    pull_request)
      target="In Review"
      if ! items="$(items_closed_by_change "$repository" "$(jq -r '.number' "$event" 2>/dev/null)")"; then
        echo "planning: the items this change closes could not be read; nothing moved"
        return 0
      fi
      ;;
    *)
      echo "planning: nothing to do for $event_name"
      return 0
      ;;
  esac

  if [[ -z "$items" ]]; then
    echo "planning: this change names no item"
    return 0
  fi

  local item
  while read -r item; do
    [[ -z "$item" ]] && continue
    move "$item" "$target" "$organisation" "$project"
  done <<<"$items"
  return 0
}

# Sourced by its own checks, which ask its parts what they would do; run, it does it.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
